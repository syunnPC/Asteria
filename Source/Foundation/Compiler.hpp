// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_FOUNDATION_COMPILER_HPP_INCLUDED
#define ASTERIA_FOUNDATION_COMPILER_HPP_INCLUDED

// Compiler abstraction for nucleus code. GCC builds every target image and the
// hosted GCC profiles; Clang builds only the hosted fuzzing and coverage
// profiles. Feature detection selects the supported attributes and builtins.

// Keeps a function out of line and, where the compiler supports GCC's noipa,
// hides its body from interprocedural analysis of its callers, so checked
// entry and stack boundaries survive optimization. Clang has no noipa and
// receives noinline alone.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(gnu::noipa)
#define ASTERIA_NOINLINE_NOIPA [[gnu::noinline, gnu::noipa]]
#endif
#endif
#if !defined(ASTERIA_NOINLINE_NOIPA)
#define ASTERIA_NOINLINE_NOIPA [[gnu::noinline]]
#endif

// Sets the padding bits of the trivially copyable object at `pointer` to zero
// so that a byte-wise copy of the object is fully determined. Only hosted Clang
// builds (fuzzing, coverage) lack __builtin_clear_padding; there the statement
// does nothing. The only consumer packs a diagnostic snapshot into atomic words
// whose padding bytes its readers ignore.
#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
#define ASTERIA_CLEAR_PADDING(pointer) __builtin_clear_padding(pointer)
#endif
#endif
#if !defined(ASTERIA_CLEAR_PADDING)
#define ASTERIA_CLEAR_PADDING(pointer) static_cast<void>(pointer)
#endif

#endif // ASTERIA_FOUNDATION_COMPILER_HPP_INCLUDED
