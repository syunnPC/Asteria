// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_LIBC_INTTYPES_H_INCLUDED
#define ASTERIA_LIBC_INTTYPES_H_INCLUDED

#include "asteria_libc.h"
#include <stdint.h>

ASTERIA_LIBC_BEGIN
typedef struct
{
    intmax_t quot;
    intmax_t rem;
} ASTERIA_LIBC_TYPE(imaxdiv_t);
ASTERIA_LIBC_NODISCARD intmax_t ASTERIA_LIBC_NAME(strtoimax)(const char* text, char** end,
                                                             int base) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD uintmax_t ASTERIA_LIBC_NAME(strtoumax)(const char* text, char** end,
                                                              int base) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD intmax_t ASTERIA_LIBC_NAME(imaxabs)(intmax_t value) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD ASTERIA_LIBC_TYPE(imaxdiv_t)
    ASTERIA_LIBC_NAME(imaxdiv)(intmax_t numerator, intmax_t denominator) ASTERIA_LIBC_NOEXCEPT;
#ifndef PRId8
#define PRId8 "d"
#endif
#ifndef PRId16
#define PRId16 "d"
#endif
#ifndef PRId32
#define PRId32 "d"
#endif
#ifndef PRId64
#define PRId64 "ld"
#endif
#ifndef PRIdMAX
#define PRIdMAX "ld"
#endif
#ifndef PRIdPTR
#define PRIdPTR "ld"
#endif
#ifndef PRIdLEAST8
#define PRIdLEAST8 "d"
#endif
#ifndef PRIdFAST8
#define PRIdFAST8 "d"
#endif
#ifndef PRIdLEAST16
#define PRIdLEAST16 "d"
#endif
#ifndef PRIdFAST16
#if __INT_FAST16_WIDTH__ == 64
#define PRIdFAST16 "ld"
#else
#define PRIdFAST16 "d"
#endif
#endif
#ifndef PRIdLEAST32
#define PRIdLEAST32 "d"
#endif
#ifndef PRIdFAST32
#if __INT_FAST32_WIDTH__ == 64
#define PRIdFAST32 "ld"
#else
#define PRIdFAST32 "d"
#endif
#endif
#ifndef PRIdLEAST64
#define PRIdLEAST64 "ld"
#endif
#ifndef PRIdFAST64
#define PRIdFAST64 "ld"
#endif
#ifndef PRIi8
#define PRIi8 "i"
#endif
#ifndef PRIi16
#define PRIi16 "i"
#endif
#ifndef PRIi32
#define PRIi32 "i"
#endif
#ifndef PRIi64
#define PRIi64 "li"
#endif
#ifndef PRIiMAX
#define PRIiMAX "li"
#endif
#ifndef PRIiPTR
#define PRIiPTR "li"
#endif
#ifndef PRIiLEAST8
#define PRIiLEAST8 "i"
#endif
#ifndef PRIiFAST8
#define PRIiFAST8 "i"
#endif
#ifndef PRIiLEAST16
#define PRIiLEAST16 "i"
#endif
#ifndef PRIiFAST16
#if __INT_FAST16_WIDTH__ == 64
#define PRIiFAST16 "li"
#else
#define PRIiFAST16 "i"
#endif
#endif
#ifndef PRIiLEAST32
#define PRIiLEAST32 "i"
#endif
#ifndef PRIiFAST32
#if __INT_FAST32_WIDTH__ == 64
#define PRIiFAST32 "li"
#else
#define PRIiFAST32 "i"
#endif
#endif
#ifndef PRIiLEAST64
#define PRIiLEAST64 "li"
#endif
#ifndef PRIiFAST64
#define PRIiFAST64 "li"
#endif
#ifndef PRIo8
#define PRIo8 "o"
#endif
#ifndef PRIo16
#define PRIo16 "o"
#endif
#ifndef PRIo32
#define PRIo32 "o"
#endif
#ifndef PRIo64
#define PRIo64 "lo"
#endif
#ifndef PRIoMAX
#define PRIoMAX "lo"
#endif
#ifndef PRIoPTR
#define PRIoPTR "lo"
#endif
#ifndef PRIoLEAST8
#define PRIoLEAST8 "o"
#endif
#ifndef PRIoFAST8
#define PRIoFAST8 "o"
#endif
#ifndef PRIoLEAST16
#define PRIoLEAST16 "o"
#endif
#ifndef PRIoFAST16
#if __INT_FAST16_WIDTH__ == 64
#define PRIoFAST16 "lo"
#else
#define PRIoFAST16 "o"
#endif
#endif
#ifndef PRIoLEAST32
#define PRIoLEAST32 "o"
#endif
#ifndef PRIoFAST32
#if __INT_FAST32_WIDTH__ == 64
#define PRIoFAST32 "lo"
#else
#define PRIoFAST32 "o"
#endif
#endif
#ifndef PRIoLEAST64
#define PRIoLEAST64 "lo"
#endif
#ifndef PRIoFAST64
#define PRIoFAST64 "lo"
#endif
#ifndef PRIu8
#define PRIu8 "u"
#endif
#ifndef PRIu16
#define PRIu16 "u"
#endif
#ifndef PRIu32
#define PRIu32 "u"
#endif
#ifndef PRIu64
#define PRIu64 "lu"
#endif
#ifndef PRIuMAX
#define PRIuMAX "lu"
#endif
#ifndef PRIuPTR
#define PRIuPTR "lu"
#endif
#ifndef PRIuLEAST8
#define PRIuLEAST8 "u"
#endif
#ifndef PRIuFAST8
#define PRIuFAST8 "u"
#endif
#ifndef PRIuLEAST16
#define PRIuLEAST16 "u"
#endif
#ifndef PRIuFAST16
#if __INT_FAST16_WIDTH__ == 64
#define PRIuFAST16 "lu"
#else
#define PRIuFAST16 "u"
#endif
#endif
#ifndef PRIuLEAST32
#define PRIuLEAST32 "u"
#endif
#ifndef PRIuFAST32
#if __INT_FAST32_WIDTH__ == 64
#define PRIuFAST32 "lu"
#else
#define PRIuFAST32 "u"
#endif
#endif
#ifndef PRIuLEAST64
#define PRIuLEAST64 "lu"
#endif
#ifndef PRIuFAST64
#define PRIuFAST64 "lu"
#endif
#ifndef PRIx8
#define PRIx8 "x"
#endif
#ifndef PRIx16
#define PRIx16 "x"
#endif
#ifndef PRIx32
#define PRIx32 "x"
#endif
#ifndef PRIx64
#define PRIx64 "lx"
#endif
#ifndef PRIxMAX
#define PRIxMAX "lx"
#endif
#ifndef PRIxPTR
#define PRIxPTR "lx"
#endif
#ifndef PRIxLEAST8
#define PRIxLEAST8 "x"
#endif
#ifndef PRIxFAST8
#define PRIxFAST8 "x"
#endif
#ifndef PRIxLEAST16
#define PRIxLEAST16 "x"
#endif
#ifndef PRIxFAST16
#if __INT_FAST16_WIDTH__ == 64
#define PRIxFAST16 "lx"
#else
#define PRIxFAST16 "x"
#endif
#endif
#ifndef PRIxLEAST32
#define PRIxLEAST32 "x"
#endif
#ifndef PRIxFAST32
#if __INT_FAST32_WIDTH__ == 64
#define PRIxFAST32 "lx"
#else
#define PRIxFAST32 "x"
#endif
#endif
#ifndef PRIxLEAST64
#define PRIxLEAST64 "lx"
#endif
#ifndef PRIxFAST64
#define PRIxFAST64 "lx"
#endif
#ifndef PRIX8
#define PRIX8 "X"
#endif
#ifndef PRIX16
#define PRIX16 "X"
#endif
#ifndef PRIX32
#define PRIX32 "X"
#endif
#ifndef PRIX64
#define PRIX64 "lX"
#endif
#ifndef PRIXMAX
#define PRIXMAX "lX"
#endif
#ifndef PRIXPTR
#define PRIXPTR "lX"
#endif
#ifndef PRIXLEAST8
#define PRIXLEAST8 "X"
#endif
#ifndef PRIXFAST8
#define PRIXFAST8 "X"
#endif
#ifndef PRIXLEAST16
#define PRIXLEAST16 "X"
#endif
#ifndef PRIXFAST16
#if __INT_FAST16_WIDTH__ == 64
#define PRIXFAST16 "lX"
#else
#define PRIXFAST16 "X"
#endif
#endif
#ifndef PRIXLEAST32
#define PRIXLEAST32 "X"
#endif
#ifndef PRIXFAST32
#if __INT_FAST32_WIDTH__ == 64
#define PRIXFAST32 "lX"
#else
#define PRIXFAST32 "X"
#endif
#endif
#ifndef PRIXLEAST64
#define PRIXLEAST64 "lX"
#endif
#ifndef PRIXFAST64
#define PRIXFAST64 "lX"
#endif
#ifndef SCNd8
#define SCNd8 "hhd"
#endif
#ifndef SCNd16
#define SCNd16 "hd"
#endif
#ifndef SCNd32
#define SCNd32 "d"
#endif
#ifndef SCNd64
#define SCNd64 "ld"
#endif
#ifndef SCNdMAX
#define SCNdMAX "ld"
#endif
#ifndef SCNdPTR
#define SCNdPTR "ld"
#endif
#ifndef SCNdLEAST8
#define SCNdLEAST8 "hhd"
#endif
#ifndef SCNdFAST8
#define SCNdFAST8 "hhd"
#endif
#ifndef SCNdLEAST16
#define SCNdLEAST16 "hd"
#endif
#ifndef SCNdFAST16
#if __INT_FAST16_WIDTH__ == 64
#define SCNdFAST16 "ld"
#else
#define SCNdFAST16 "d"
#endif
#endif
#ifndef SCNdLEAST32
#define SCNdLEAST32 "d"
#endif
#ifndef SCNdFAST32
#if __INT_FAST32_WIDTH__ == 64
#define SCNdFAST32 "ld"
#else
#define SCNdFAST32 "d"
#endif
#endif
#ifndef SCNdLEAST64
#define SCNdLEAST64 "ld"
#endif
#ifndef SCNdFAST64
#define SCNdFAST64 "ld"
#endif
#ifndef SCNi8
#define SCNi8 "hhi"
#endif
#ifndef SCNi16
#define SCNi16 "hi"
#endif
#ifndef SCNi32
#define SCNi32 "i"
#endif
#ifndef SCNi64
#define SCNi64 "li"
#endif
#ifndef SCNiMAX
#define SCNiMAX "li"
#endif
#ifndef SCNiPTR
#define SCNiPTR "li"
#endif
#ifndef SCNiLEAST8
#define SCNiLEAST8 "hhi"
#endif
#ifndef SCNiFAST8
#define SCNiFAST8 "hhi"
#endif
#ifndef SCNiLEAST16
#define SCNiLEAST16 "hi"
#endif
#ifndef SCNiFAST16
#if __INT_FAST16_WIDTH__ == 64
#define SCNiFAST16 "li"
#else
#define SCNiFAST16 "i"
#endif
#endif
#ifndef SCNiLEAST32
#define SCNiLEAST32 "i"
#endif
#ifndef SCNiFAST32
#if __INT_FAST32_WIDTH__ == 64
#define SCNiFAST32 "li"
#else
#define SCNiFAST32 "i"
#endif
#endif
#ifndef SCNiLEAST64
#define SCNiLEAST64 "li"
#endif
#ifndef SCNiFAST64
#define SCNiFAST64 "li"
#endif
#ifndef SCNo8
#define SCNo8 "hho"
#endif
#ifndef SCNo16
#define SCNo16 "ho"
#endif
#ifndef SCNo32
#define SCNo32 "o"
#endif
#ifndef SCNo64
#define SCNo64 "lo"
#endif
#ifndef SCNoMAX
#define SCNoMAX "lo"
#endif
#ifndef SCNoPTR
#define SCNoPTR "lo"
#endif
#ifndef SCNoLEAST8
#define SCNoLEAST8 "hho"
#endif
#ifndef SCNoFAST8
#define SCNoFAST8 "hho"
#endif
#ifndef SCNoLEAST16
#define SCNoLEAST16 "ho"
#endif
#ifndef SCNoFAST16
#if __INT_FAST16_WIDTH__ == 64
#define SCNoFAST16 "lo"
#else
#define SCNoFAST16 "o"
#endif
#endif
#ifndef SCNoLEAST32
#define SCNoLEAST32 "o"
#endif
#ifndef SCNoFAST32
#if __INT_FAST32_WIDTH__ == 64
#define SCNoFAST32 "lo"
#else
#define SCNoFAST32 "o"
#endif
#endif
#ifndef SCNoLEAST64
#define SCNoLEAST64 "lo"
#endif
#ifndef SCNoFAST64
#define SCNoFAST64 "lo"
#endif
#ifndef SCNu8
#define SCNu8 "hhu"
#endif
#ifndef SCNu16
#define SCNu16 "hu"
#endif
#ifndef SCNu32
#define SCNu32 "u"
#endif
#ifndef SCNu64
#define SCNu64 "lu"
#endif
#ifndef SCNuMAX
#define SCNuMAX "lu"
#endif
#ifndef SCNuPTR
#define SCNuPTR "lu"
#endif
#ifndef SCNuLEAST8
#define SCNuLEAST8 "hhu"
#endif
#ifndef SCNuFAST8
#define SCNuFAST8 "hhu"
#endif
#ifndef SCNuLEAST16
#define SCNuLEAST16 "hu"
#endif
#ifndef SCNuFAST16
#if __INT_FAST16_WIDTH__ == 64
#define SCNuFAST16 "lu"
#else
#define SCNuFAST16 "u"
#endif
#endif
#ifndef SCNuLEAST32
#define SCNuLEAST32 "u"
#endif
#ifndef SCNuFAST32
#if __INT_FAST32_WIDTH__ == 64
#define SCNuFAST32 "lu"
#else
#define SCNuFAST32 "u"
#endif
#endif
#ifndef SCNuLEAST64
#define SCNuLEAST64 "lu"
#endif
#ifndef SCNuFAST64
#define SCNuFAST64 "lu"
#endif
#ifndef SCNx8
#define SCNx8 "hhx"
#endif
#ifndef SCNx16
#define SCNx16 "hx"
#endif
#ifndef SCNx32
#define SCNx32 "x"
#endif
#ifndef SCNx64
#define SCNx64 "lx"
#endif
#ifndef SCNxMAX
#define SCNxMAX "lx"
#endif
#ifndef SCNxPTR
#define SCNxPTR "lx"
#endif
#ifndef SCNxLEAST8
#define SCNxLEAST8 "hhx"
#endif
#ifndef SCNxFAST8
#define SCNxFAST8 "hhx"
#endif
#ifndef SCNxLEAST16
#define SCNxLEAST16 "hx"
#endif
#ifndef SCNxFAST16
#if __INT_FAST16_WIDTH__ == 64
#define SCNxFAST16 "lx"
#else
#define SCNxFAST16 "x"
#endif
#endif
#ifndef SCNxLEAST32
#define SCNxLEAST32 "x"
#endif
#ifndef SCNxFAST32
#if __INT_FAST32_WIDTH__ == 64
#define SCNxFAST32 "lx"
#else
#define SCNxFAST32 "x"
#endif
#endif
#ifndef SCNxLEAST64
#define SCNxLEAST64 "lx"
#endif
#ifndef SCNxFAST64
#define SCNxFAST64 "lx"
#endif
ASTERIA_LIBC_END

#endif
