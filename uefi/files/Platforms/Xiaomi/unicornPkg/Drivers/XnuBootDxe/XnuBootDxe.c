/** @file
  Start the XNU loader from the "XNUP" payload the boot shim left in RAM.

  Why this exists: in this firmware the loader is normally started from BDS's "unable to boot" path
  (hakios-uefi's BootManager.c calls XnuLoaderMain() there). On this phone that path is not reached: with no
  OS installed BDS has nothing but its own (empty) Boot Manager menu, it boots that, and the phone sits there
  until it is reset -- which is exactly what a photo of the screen shows (a menu with no entries), and why no
  `xnuloader` ever appears on USB.

  So this driver does the handoff itself, at the end of DXE dispatch, where nothing can get in the way:

    1. check "XNUP" at XNU_PAYLOAD_BASE (the boot shim put it there; the DSC's BuildOptions say where),
    2. find the `loader.efi` entry inside the container,
    3. say what it found on the screen (the only output that exists until the loader's USB is up),
    4. LoadImage from memory and StartImage it.

  If the magic is not there, it prints that and returns: the rest of the firmware then behaves as before, so
  this cannot make a boot that used to work stop working.

  Payload layout (xnuloader/mkpayload.py):
    header : u32 magic "XNUP", u32 size
    entries: char name[16], u32 size, u32 reserved, data padded to 16
    end    : 24 zero bytes

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#ifndef XNU_PAYLOAD_BASE
#define XNU_PAYLOAD_BASE  0xF0000000ULL   /* the DSC's BuildOptions override this for a device */
#endif

#define XNUP_MAGIC        0x50554E58u     /* "XNUP" little endian */
#define XNUP_MAX_ENTRIES  32

typedef struct {
  UINT32  Magic;
  UINT32  Size;
} XNUP_HEADER;

typedef struct {
  CHAR8   Name[16];
  UINT32  Size;
  UINT32  Reserved;
} XNUP_ENTRY;

STATIC
VOID
XnuBootSay (
  IN CONST CHAR16  *Message
  )
{
  if (gST != NULL && gST->ConOut != NULL) {
    gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Message);
    gST->ConOut->OutputString (gST->ConOut, L"\r\n");
  }
}

STATIC
BOOLEAN
NameIs (
  IN CONST CHAR8  *Name,
  IN CONST CHAR8  *Want
  )
{
  UINTN  Index;

  for (Index = 0; Index < 16; Index++) {
    if (Name[Index] != Want[Index]) {
      return FALSE;
    }
    if (Want[Index] == 0) {
      return TRUE;
    }
  }
  return TRUE;
}

/**
  Driver entry point. Runs during DXE dispatch; if the payload is there, control does not come back.
**/
EFI_STATUS
EFIAPI
InitXnuBoot (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT8      *Base = (UINT8 *)(UINTN)XNU_PAYLOAD_BASE;
  XNUP_HEADER *Header;
  UINTN       Offset;
  UINTN       Entry;
  UINT8      *Loader = NULL;
  UINTN       LoaderSize = 0;
  EFI_HANDLE  Image;

  Header = (XNUP_HEADER *)Base;
  DEBUG ((DEBUG_INFO, "xnuboot: looking for XNUP at %08x (%08x)\n", (UINT32)XNU_PAYLOAD_BASE, Header->Magic));

  if (Header->Magic != XNUP_MAGIC) {
    DEBUG ((DEBUG_ERROR, "xnuboot: no XNUP magic at %08x (read %08x)\n", (UINT32)XNU_PAYLOAD_BASE, Header->Magic));
    XnuBootSay (L"xnuboot: no XNUP payload in RAM -- the boot shim did not copy it here");
    return EFI_NOT_FOUND;
  }

  XnuBootSay (L"xnuboot: XNUP payload found");

  Offset = sizeof (XNUP_HEADER);
  for (Entry = 0; Entry < XNUP_MAX_ENTRIES; Entry++) {
    XNUP_ENTRY  *Item = (XNUP_ENTRY *)(Base + Offset);

    if (Item->Size == 0 && Item->Name[0] == 0) {
      break;
    }
    DEBUG ((DEBUG_INFO, "xnuboot: entry %.16a size %d\n", Item->Name, Item->Size));
    if (NameIs (Item->Name, "loader.efi")) {
      Loader     = Base + Offset + sizeof (XNUP_ENTRY);
      LoaderSize = Item->Size;
      break;
    }
    Offset += sizeof (XNUP_ENTRY) + ALIGN_VALUE (Item->Size, 16);
  }

  if (Loader == NULL || LoaderSize == 0) {
    DEBUG ((DEBUG_ERROR, "xnuboot: no loader.efi entry in the payload\n"));
    XnuBootSay (L"xnuboot: payload has no loader.efi entry");
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "xnuboot: loader.efi at %p, %d bytes -- starting it\n", Loader, LoaderSize));
  XnuBootSay (L"xnuboot: starting loader.efi");

  Status = gBS->LoadImage (TRUE, ImageHandle, NULL, Loader, LoaderSize, &Image);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "xnuboot: LoadImage failed (%r)\n", Status));
    XnuBootSay (L"xnuboot: LoadImage FAILED");
    return Status;
  }

  Status = gBS->StartImage (Image, NULL, NULL);

  /* the loader only comes back if it decided not to hand over to the kernel */
  DEBUG ((DEBUG_ERROR, "xnuboot: StartImage returned (%r)\n", Status));
  XnuBootSay (L"xnuboot: the loader returned -- see the screen above");

  return Status;
}
