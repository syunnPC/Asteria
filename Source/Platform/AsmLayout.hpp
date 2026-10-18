// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_ASM_LAYOUT_HPP_INCLUDED
#define ASTERIA_PLATFORM_ASM_LAYOUT_HPP_INCLUDED

// Layout units (Source/Platform/<Isa>/Layout/<Name>.cpp) state the constants that hand-written
// assembly takes from C++ types. The target build compiles each unit to assembly with the kernel's
// compiler, flags and definitions, and Tools/Build/GenerateAsmLayout.py turns every marker into a
// #define of Generated/Platform/<Isa>/<Name>.h, which the.S files include. The values are therefore
// those of the C++ layout the kernel is built with. ASTERIA_ASM_LAYOUT(NAME, value) emits one
// marker inside a function body; `value` must be an integer constant expression.
#define ASTERIA_ASM_LAYOUT(name, value) __asm__ volatile("\n.ascii \"@ASTERIA-ASM-LAYOUT " #name " %0\"" : : "i"(value))

#endif // ASTERIA_PLATFORM_ASM_LAYOUT_HPP_INCLUDED
