/** @file
  USB serial console for unicorn: a CDC ACM device on top of the firmware's EFI_USBFN_IO_PROTOCOL.

  Why: until this existed, this firmware had no way to say anything except the screen. The loader's USB
  fastboot path needs Qualcomm's EFI_USB_DEVICE_PROTOCOL, which a blob-free unicornPkg does not build
  (xnuloader/loader.c prints "usb: device protocol not found" and carries on). The *standard*
  EFI_USBFN_IO_PROTOCOL is what this firmware can provide from source -- UsbfnDwc3Dxe does -- and this
  driver is the class on top of it: the PC sees a serial port, and everything this firmware prints can be
  read there as text.

  Shape: this driver owns no hardware. UsbfnDwc3Dxe owns the DWC3, PlatformUsbfnLib owns the board
  bring-up (docs/USB-SERIAL.md). Here there are only descriptors, the class requests, the bulk data path
  and a bounded transmit ring.

  Deliberate limits of this version:
    - transmit only (host input on the bulk OUT endpoint is drained and ignored);
    - every flush gives up after a bounded time, so an absent or stalled host can never wedge the
      firmware; characters are dropped, never buffered without limit;
    - USB 2.0, one configuration, no serial-state notifications.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/SerialIo.h>
#include <Protocol/UsbFunctionIo.h>

#define USBSER_EP_NOTIFY   1u   /* interrupt IN, ep 0x81 */
#define USBSER_EP_DATA     2u   /* bulk IN/OUT,  ep 0x82 / 0x02 */

#define USBSER_TX_RING     1024
#define USBSER_CHUNK       512
#define USBSER_POLL_US     10000  /* timer period */

STATIC EFI_USBFN_IO_PROTOCOL  *mUsbfn;
STATIC BOOLEAN                 mStarted;
STATIC BOOLEAN                 mConfigured;
STATIC BOOLEAN                 mHostOpen;
STATIC BOOLEAN                 mTxBusy;
STATIC VOID                   *mRxBuffer;
STATIC VOID                   *mTxBuffer;
STATIC UINT8                   mTxRing[USBSER_TX_RING];
STATIC UINTN                   mTxRingLen;
STATIC UINT8                   mLineCoding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 }; /* 115200 8N1 */
STATIC EFI_SERIAL_IO_MODE      mSerialMode;
STATIC EFI_SERIAL_IO_PROTOCOL  mSerialIo;

/* ------------------------------------------------------------------------ */
/* The device: vid:pid 18d1:d00d, "unicorn loader", CDC ACM                  */

STATIC CONST UINT8  mDeviceDesc[18] = {
  18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
  0xD1, 0x18, 0x0D, 0xD0, 0x00, 0x01,
  1, 2, 3, 1
};

STATIC CONST UINT8  mConfigDesc[67] = {
  /*  0: configuration */            9, 2, 67, 0, 2, 1, 0, 0x80, 50,
  /*  9: interface 0, CDC control */ 9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
  /* 18: header functional */        5, 0x24, 0x00, 0x10, 0x01,
  /* 23: call management */          5, 0x24, 0x01, 0x00, 1,
  /* 28: ACM */                      4, 0x24, 0x02, 0x02,
  /* 32: union */                    5, 0x24, 0x06, 0, 1,
  /* 37: notification, ep 0x81 */    7, 5, 0x81, 0x03, 0x10, 0x00, 0x10,
  /* 44: interface 1, CDC data */    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
  /* 53: bulk out, ep 0x02 */        7, 5, 0x02, 0x02, 0x00, 0x02, 0x00,
  /* 60: bulk in, ep 0x82 */         7, 5, 0x82, 0x02, 0x00, 0x02, 0x00
};

#define CFG_OFF_IF0       9
#define CFG_OFF_EP_NOTIFY 37
#define CFG_OFF_IF1       44
#define CFG_OFF_EP_OUT    53
#define CFG_OFF_EP_IN     60

STATIC CONST UINT8  mString0[4]  = { 4, 3, 0x09, 0x04 };
STATIC CONST UINT8  mString1[14] = { 14, 3, 'H',0, 'a',0, 'k',0, 'i',0, 'O',0, 'S',0 };
STATIC CONST UINT8  mString2[34] = { 34, 3, 'u',0, 'n',0, 'i',0, 'c',0, 'o',0, 'r',0, 'n',0, ' ',0,
                                     'l',0, 'o',0, 'a',0, 'd',0, 'e',0, 'r',0 };
STATIC CONST UINT8  mString3[20] = { 20, 3, 'x',0, 'n',0, 'u',0, 'l',0, 'o',0, 'a',0, 'd',0, 'e',0, 'r',0 };

STATIC EFI_USB_ENDPOINT_DESCRIPTOR  *mEpsIf0[2];
STATIC EFI_USB_ENDPOINT_DESCRIPTOR  *mEpsIf1[3];
STATIC EFI_USB_INTERFACE_INFO        mIf0;
STATIC EFI_USB_INTERFACE_INFO        mIf1;
STATIC EFI_USB_INTERFACE_INFO       *mIfTable[3];
STATIC EFI_USB_CONFIG_INFO           mConfig;
STATIC EFI_USB_CONFIG_INFO          *mConfigTable[2];
STATIC EFI_USB_DEVICE_INFO           mDeviceInfo;

/* ------------------------------------------------------------------------ */

VOID
UsbSerialPump (
  VOID
  )
{
  UINTN  Round;

  for (Round = 0; Round < 64; Round++) {
    EFI_USBFN_MESSAGE          Message = EfiUsbMsgNone;
    UINTN                      PayloadSize = sizeof (EFI_USBFN_MESSAGE_PAYLOAD);
    EFI_USBFN_MESSAGE_PAYLOAD  Payload;
    EFI_STATUS                 Status;

    ZeroMem (&Payload, sizeof (Payload));

    Status = mUsbfn->EventHandler (mUsbfn, &Message, &PayloadSize, &Payload);
    if (EFI_ERROR (Status) || Message == EfiUsbMsgNone) {
      return;
    }

    switch (Message) {
      case EfiUsbMsgEndpointStatusChangedTx:
        if (Payload.utr.EndpointIndex == USBSER_EP_DATA) {
          mTxBusy = FALSE;
        }
        break;

      case EfiUsbMsgEndpointStatusChangedRx:
        if (Payload.utr.EndpointIndex == USBSER_EP_DATA && mConfigured) {
          UINTN Size = USBSER_CHUNK;
          mUsbfn->Transfer (mUsbfn, USBSER_EP_DATA, EfiUsbEndpointDirectionDeviceRx, &Size, mRxBuffer);
        }
        break;

      case EfiUsbMsgBusEventReset:
      case EfiUsbMsgBusEventDetach:
        mConfigured = FALSE;
        mHostOpen   = FALSE;
        mTxBusy     = FALSE;
        break;

      default:
        break;
    }
  }
}

STATIC
VOID
UsbSerialControlSend (
  IN CONST VOID  *Data,
  IN UINTN        Length
  )
{
  UINTN  Size = Length;

  if (!EFI_ERROR (mUsbfn->Transfer (mUsbfn, 0, EfiUsbEndpointDirectionDeviceTx, &Size, (VOID *)Data))) {
    UsbSerialPump ();
  }
}

STATIC
VOID
UsbSerialControlReceive (
  OUT VOID   *Data,
  IN  UINTN   Length
  )
{
  UINTN  Size = Length;

  if (!EFI_ERROR (mUsbfn->Transfer (mUsbfn, 0, EfiUsbEndpointDirectionDeviceRx, &Size, Data))) {
    UsbSerialPump ();
  }
}

STATIC
VOID
UsbSerialGetDescriptor (
  IN EFI_USB_DEVICE_REQUEST  *Request
  )
{
  CONST UINT8  *Desc = NULL;
  UINTN         Length = 0;
  UINTN         Want = Request->Length;

  switch (Request->Value >> 8) {
    case 1: Desc = mDeviceDesc; Length = sizeof (mDeviceDesc); break;
    case 2: Desc = mConfigDesc; Length = sizeof (mConfigDesc); break;
    case 3:
      switch (Request->Value & 0xFF) {
        case 0: Desc = mString0; Length = sizeof (mString0); break;
        case 1: Desc = mString1; Length = sizeof (mString1); break;
        case 2: Desc = mString2; Length = sizeof (mString2); break;
        case 3: Desc = mString3; Length = sizeof (mString3); break;
        default: break;
      }
      break;
    default:
      break;
  }

  if (Desc == NULL) {
    /* e.g. the device_qualifier of a high speed only device: the state is in/out, hence the pointer */
    BOOLEAN Stall = TRUE;
    mUsbfn->SetEndpointStallState (mUsbfn, 0, EfiUsbEndpointDirectionDeviceTx, &Stall);
    return;
  }
  if ((Want != 0) && (Want < Length)) {
    Length = Want;
  }
  UsbSerialControlSend (Desc, Length);
}

STATIC
VOID
UsbSerialHandleSetup (
  IN EFI_USB_DEVICE_REQUEST  *Request
  )
{
  UINT8  Type = (UINT8)(Request->RequestType & 0x60);
  UINT8  In   = (UINT8)(Request->RequestType & 0x80);

  if ((Type == 0x00) && (In != 0)) {
    switch (Request->Request) {
      case 0x06: UsbSerialGetDescriptor (Request); break;
      case 0x08: { UINT8 Value = 1; UsbSerialControlSend (&Value, 1); } break;
      case 0x00: { UINT8 Zero[2] = { 0, 0 }; UsbSerialControlSend (Zero, 2); } break;
      case 0x0A: { UINT8 Zero = 0; UsbSerialControlSend (&Zero, 1); } break;
      default: break;
    }
    return;
  }

  if ((Type == 0x00) && (In == 0)) {
    if (Request->Request == 0x09) {
      /* SET_CONFIGURATION: arm the host->device bulk endpoint */
      UINTN Size = USBSER_CHUNK;
      mConfigured = TRUE;
      mUsbfn->Transfer (mUsbfn, USBSER_EP_DATA, EfiUsbEndpointDirectionDeviceRx, &Size, mRxBuffer);
    }
    /* SET_INTERFACE / SET_FEATURE / CLEAR_FEATURE: the driver answers the status stage itself */
    return;
  }

  if (Type == 0x20) {
    switch (Request->Request) {
      case 0x20: UsbSerialControlReceive (mLineCoding, sizeof (mLineCoding)); break;
      case 0x21: UsbSerialControlSend (mLineCoding, sizeof (mLineCoding)); break;
      case 0x22: mHostOpen = ((Request->Value & 1) != 0); break;
      default: break;
    }
  }
}

/* ------------------------------------------------------------------------ */
/* Transmit                                                                  */

STATIC
VOID
UsbSerialFlush (
  VOID
  )
{
  UINTN   Index;
  UINTN   Count;
  UINTN   Size;
  UINT8  *Dst;

  if (!mStarted || !mConfigured || (mTxRingLen == 0)) {
    return;
  }

  if (!mTxBusy) {
    Count = (mTxRingLen < USBSER_CHUNK) ? mTxRingLen : USBSER_CHUNK;
    Dst   = (UINT8 *)mTxBuffer;
    for (Index = 0; Index < Count; Index++) {
      Dst[Index] = mTxRing[Index];
    }
    for (Index = Count; Index < mTxRingLen; Index++) {
      mTxRing[Index - Count] = mTxRing[Index];
    }
    mTxRingLen -= Count;

    Size    = Count;
    mTxBusy = TRUE;
    if (EFI_ERROR (mUsbfn->Transfer (mUsbfn, USBSER_EP_DATA, EfiUsbEndpointDirectionDeviceTx, &Size, mTxBuffer))) {
      mTxBusy = FALSE;
      return;
    }
  }

  /* bounded: a host that stopped reading must never stall the firmware */
  for (Index = 0; (Index < 40) && mTxBusy; Index++) {
    UsbSerialPump ();
    if (mTxBusy) {
      MicroSecondDelay (1000);
    }
  }
}

/**
  Writes text to the USB serial port. Safe to call before the host has opened the port: the text is
  simply dropped, and nothing blocks.
**/
VOID
UsbSerialWrite (
  IN CONST CHAR8  *Buffer,
  IN UINTN         Length
  )
{
  if (!mStarted || !mConfigured || !mHostOpen) {
    return;
  }

  while (Length-- != 0) {
    if (mTxRingLen < sizeof (mTxRing)) {
      mTxRing[mTxRingLen++] = (UINT8)*Buffer;
    }
    Buffer++;
    if (mTxRingLen >= sizeof (mTxRing)) {
      UsbSerialFlush ();
    }
  }
  UsbSerialFlush ();
}

/* ------------------------------------------------------------------------ */
/* EFI_SERIAL_IO_PROTOCOL                                                    */

STATIC
EFI_STATUS
EFIAPI
SerialReset (
  IN EFI_SERIAL_IO_PROTOCOL  *This
  )
{
  mTxRingLen = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialSetAttributes (
  IN EFI_SERIAL_IO_PROTOCOL  *This,
  IN UINT64                   BaudRate,
  IN UINT32                   ReceiveFifoDepth,
  IN UINT32                   Timeout,
  IN EFI_PARITY_TYPE          Parity,
  IN UINT8                    DataBits,
  IN EFI_STOP_BITS_TYPE       StopBits
  )
{
  return EFI_UNSUPPORTED;   /* the USB side runs at the bus speed; the CDC line coding is advisory */
}

STATIC
EFI_STATUS
EFIAPI
SerialSetControl (
  IN EFI_SERIAL_IO_PROTOCOL  *This,
  IN UINT32                   Control
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
SerialGetControl (
  IN  EFI_SERIAL_IO_PROTOCOL  *This,
  OUT UINT32                  *Control
  )
{
  if (Control == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Control = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialWrite (
  IN     EFI_SERIAL_IO_PROTOCOL  *This,
  IN OUT UINTN                   *BufferSize,
  IN     VOID                    *Buffer
  )
{
  if ((BufferSize == NULL) || ((Buffer == NULL) && (*BufferSize != 0))) {
    return EFI_INVALID_PARAMETER;
  }
  UsbSerialWrite ((CONST CHAR8 *)Buffer, *BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialRead (
  IN     EFI_SERIAL_IO_PROTOCOL  *This,
  IN OUT UINTN                   *BufferSize,
  OUT    VOID                    *Buffer
  )
{
  /* transmit-only console: nothing is buffered for the host's input */
  if (BufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *BufferSize = 0;
  return EFI_TIMEOUT;
}

/* ------------------------------------------------------------------------ */

STATIC
VOID
EFIAPI
UsbSerialTimer (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  UsbSerialPump ();
  if (mTxRingLen != 0) {
    UsbSerialFlush ();
  }
}

/**
  Driver entry point: bring the controller up through EFI_USBFN_IO_PROTOCOL, present a CDC ACM device and
  keep it serviced from a periodic timer.
**/
EFI_STATUS
EFIAPI
InitUsbSerial (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Timer;

  Status = gBS->LocateProtocol (&gEfiUsbFunctionIoProtocolGuid, NULL, (VOID **)&mUsbfn);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "usbserial: no EFI_USBFN_IO_PROTOCOL (%r), screen only\n", Status));
    return Status;
  }

  /* the descriptor tables the function driver binds endpoints from */
  mEpsIf0[0]      = (EFI_USB_ENDPOINT_DESCRIPTOR *)(mConfigDesc + CFG_OFF_EP_NOTIFY);
  mEpsIf0[1]      = NULL;
  mEpsIf1[0]      = (EFI_USB_ENDPOINT_DESCRIPTOR *)(mConfigDesc + CFG_OFF_EP_OUT);
  mEpsIf1[1]      = (EFI_USB_ENDPOINT_DESCRIPTOR *)(mConfigDesc + CFG_OFF_EP_IN);
  mEpsIf1[2]      = NULL;
  mIf0.InterfaceDescriptor      = (EFI_USB_INTERFACE_DESCRIPTOR *)(mConfigDesc + CFG_OFF_IF0);
  mIf0.EndpointDescriptorTable  = mEpsIf0;
  mIf1.InterfaceDescriptor      = (EFI_USB_INTERFACE_DESCRIPTOR *)(mConfigDesc + CFG_OFF_IF1);
  mIf1.EndpointDescriptorTable  = mEpsIf1;
  mIfTable[0]     = &mIf0;
  mIfTable[1]     = &mIf1;
  mIfTable[2]     = NULL;
  mConfig.ConfigDescriptor      = (EFI_USB_CONFIG_DESCRIPTOR *)mConfigDesc;
  mConfig.InterfaceInfoTable    = mIfTable;
  mConfigTable[0] = &mConfig;
  mConfigTable[1] = NULL;
  mDeviceInfo.DeviceDescriptor  = (EFI_USB_DEVICE_DESCRIPTOR *)mDeviceDesc;
  mDeviceInfo.ConfigInfoTable   = mConfigTable;

  Status = mUsbfn->StartController (mUsbfn);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "usbserial: StartController failed (%r)\n", Status));
    return Status;
  }
  DEBUG ((DEBUG_INFO, "usbserial: controller started\n"));

  Status = mUsbfn->ConfigureEnableEndpoints (mUsbfn, &mDeviceInfo);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "usbserial: ConfigureEnableEndpoints failed (%r)\n", Status));
    return Status;
  }
  DEBUG ((DEBUG_INFO, "usbserial: endpoints bound, CDC ACM waiting for a host\n"));

  if (EFI_ERROR (mUsbfn->AllocateTransferBuffer (mUsbfn, USBSER_CHUNK, &mRxBuffer)) ||
      EFI_ERROR (mUsbfn->AllocateTransferBuffer (mUsbfn, USBSER_CHUNK, &mTxBuffer))) {
    DEBUG ((DEBUG_ERROR, "usbserial: no DMA buffers\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  mStarted   = TRUE;
  mTxRingLen = 0;

  mSerialMode.BaudRate         = 115200;
  mSerialMode.ReceiveFifoDepth = 1;
  mSerialMode.Timeout          = 1;
  mSerialMode.Parity           = NoParity;
  mSerialMode.DataBits         = 8;
  mSerialMode.StopBits         = OneStopBit;

  mSerialIo.Revision        = EFI_SERIAL_IO_PROTOCOL_REVISION;
  mSerialIo.Reset           = SerialReset;
  mSerialIo.SetAttributes   = SerialSetAttributes;
  mSerialIo.SetControl      = SerialSetControl;
  mSerialIo.GetControl      = SerialGetControl;
  mSerialIo.Write           = SerialWrite;
  mSerialIo.Read            = SerialRead;
  mSerialIo.Mode            = &mSerialMode;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEfiSerialIoProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mSerialIo
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "usbserial: EFI_SERIAL_IO_PROTOCOL not installed (%r)\n", Status));
  }

  /* keep the CDC data stage serviced while the rest of the firmware runs */
  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, UsbSerialTimer, NULL, &Timer);
  if (!EFI_ERROR (Status)) {
    gBS->SetTimer (Timer, TimerPeriodic, USBSER_POLL_US);
  }

  DEBUG ((DEBUG_INFO, "usbserial: up\n"));
  UsbSerialWrite ("unicorn: USB serial console up\r\n", 32);
  return EFI_SUCCESS;
}
