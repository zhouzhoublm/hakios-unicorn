/*
 * devices/unicorn/uefi/files/BootShim/BootShim.S
 *
 * Mu-Silicium's BootShim.S with HakiOS's "XNUP" payload copy (hakios-uefi's hakios.patch), plus one fix
 * this port needed: the literal pool of `ldr x5, =FD_BASE` / `ldr x6, =FD_SIZE` is emitted by the
 * assembler at the END of the section, which is exactly where `_Payload` is -- so BootShim.bin came out
 * 16 bytes LONGER than the address it copied the firmware volume from, and every FD arrived at FD_BASE
 * shifted by 16 bytes (an invalid FV header: the phone resets, no display, no USB). `.ltorg` flushes the
 * pool before `_Payload`, which makes the copy source and the FD's real position the same address.
 *
 * Check it after any change (tools/pack-image.py does):
 *   aarch64-linux-gnu-objdump -D -b binary -m aarch64 BootShim.bin | sed -n '8p'   # adr x1, 0xXXX
 *   stat -c%s BootShim.bin                                                         # must be 0xXXX
 */
_Head:
  adr   x1, _Payload    /* Set _Entry address */
  b     _Start          /* Jump to the real code */

#if REQUIRES_KERNEL_HEADER == 1
_StackBase:
  .quad FD_BASE         /* Text Offset */

_StackSize:
  .quad FD_SIZE         /* Image Size */
  .quad 0               /* Flags */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .ascii "ARM\x64"      /* ARM64 Magic */
  .long 0               /* Reserved */
#endif

_Start:
  mov   x4, x1
  ldr   x5, =FD_BASE
  cmp   x4, x5
  beq   _Entry
  ldr   x6,  =FD_SIZE

_CopyLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyLoop

#ifdef PAYLOAD_BASE
  /* x4 now points just past the FD: an optional "XNUP" payload may follow */
  ldr   w2, [x4]
  movz  w3, #0x4E58
  movk  w3, #0x5055, lsl #16
  cmp   w2, w3
  b.ne  _NoPayload
  ldr   w6, [x4, #4]    /* Payload Size */
  add   x6, x6, #23     /* Header + Round Up to 16 */
  and   x6, x6, #~15
  movz  x5, #(PAYLOAD_BASE >> 16), lsl #16

_PayloadLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _PayloadLoop

_NoPayload:
#endif
  ldr   x5, =FD_BASE

_Entry:
  br    x5

_Dead:
  b     _Dead

  .ltorg

.text
.align 4

_Payload:
