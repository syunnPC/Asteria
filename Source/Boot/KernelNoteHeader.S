// SPDX-License-Identifier: Apache-2.0

// ELF note header and permission padding; values come from the kernel ABI.
#if defined(ASTERIA_TARGET_AMD64)
#include "Generated/Platform/Amd64/KernelNoteAsmLayout.h"
#else
#include "Generated/Platform/Ia64/KernelNoteAsmLayout.h"
#endif
.section .asteria_kernel_note,"a",@note
.long 8
.long ASTERIA_KERNEL_NOTE_DESCRIPTOR_BYTES
.long ASTERIA_KERNEL_NOTE_TYPE
.ascii "ASTERIA\0"
.section .asteria_text_padding,"ax",@nobits
.space 1
.section .asteria_ro_padding,"a",@nobits
.space 1
.section .asteria_ro_tail,"a",@nobits
.space 1
