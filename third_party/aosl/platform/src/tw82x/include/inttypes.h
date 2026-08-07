#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

/* Check if __STDC_FORMAT_MACROS is defined for C++ compatibility */
#ifdef __cplusplus
#  ifndef __STDC_FORMAT_MACROS
#    define __STDC_FORMAT_MACROS
#  endif
#endif

/* Format macros for signed integers */
#ifndef PRId8
#  define PRId8       "d"
#endif
#ifndef PRId16
#  define PRId16      "d"
#endif
#ifndef PRId32
#  ifdef __LP64__
#    define PRId32    "d"
#  else
#    define PRId32    "ld"
#  endif
#endif
#ifndef PRId64
#  define PRId64      "lld"
#endif

#ifndef PRIi8
#  define PRIi8       "i"
#endif
#ifndef PRIi16
#  define PRIi16      "i"
#endif
#ifndef PRIi32
#  ifdef __LP64__
#    define PRIi32    "i"
#  else
#    define PRIi32    "li"
#  endif
#endif
#ifndef PRIi64
#  define PRIi64      "lli"
#endif

/* Format macros for unsigned integers */
#ifndef PRIu8
#  define PRIu8       "u"
#endif
#ifndef PRIu16
#  define PRIu16      "u"
#endif
#ifndef PRIu32
#  ifdef __LP64__
#    define PRIu32    "u"
#  else
#    define PRIu32    "lu"
#  endif
#endif
#ifndef PRIu64
#  define PRIu64      "llu"
#endif

#ifndef PRIo8
#  define PRIo8       "o"
#endif
#ifndef PRIo16
#  define PRIo16      "o"
#endif
#ifndef PRIo32
#  ifdef __LP64__
#    define PRIo32    "o"
#  else
#    define PRIo32    "lo"
#  endif
#endif
#ifndef PRIo64
#  define PRIo64      "llo"
#endif

#ifndef PRIx8
#  define PRIx8       "x"
#endif
#ifndef PRIx16
#  define PRIx16      "x"
#endif
#ifndef PRIx32
#  ifdef __LP64__
#    define PRIx32    "x"
#  else
#    define PRIx32    "lx"
#  endif
#endif
#ifndef PRIx64
#  define PRIx64      "llx"
#endif

#ifndef PRIX8
#  define PRIX8       "X"
#endif
#ifndef PRIX16
#  define PRIX16      "X"
#endif
#ifndef PRIX32
#  ifdef __LP64__
#    define PRIX32    "X"
#  else
#    define PRIX32    "lX"
#  endif
#endif
#ifndef PRIX64
#  define PRIX64      "llX"
#endif

/* For intptr_t and uintptr_t */
#ifdef __intptr_t_defined
#  ifndef PRIdPTR
#    define PRIdPTR   "d"
#  endif
#  ifndef PRIiPTR
#    define PRIiPTR   "i"
#  endif
#  ifndef PRIuPTR
#    define PRIuPTR   "u"
#  endif
#  ifndef PRIoPTR
#    define PRIoPTR   "o"
#  endif
#  ifndef PRIxPTR
#    define PRIxPTR   "x"
#  endif
#  ifndef PRIXPTR
#    define PRIXPTR   "X"
#  endif
#endif

/* For intmax_t and uintmax_t */
#ifndef PRIdMAX
#  define PRIdMAX     "lld"
#endif
#ifndef PRIiMAX
#  define PRIiMAX     "lli"
#endif
#ifndef PRIuMAX
#  define PRIuMAX     "llu"
#endif
#ifndef PRIoMAX
#  define PRIoMAX     "llo"
#endif
#ifndef PRIxMAX
#  define PRIxMAX     "llx"
#endif
#ifndef PRIXMAX
#  define PRIXMAX     "llX"
#endif

/* Scan format macros for signed integers */
#ifndef SCNd8
#  define SCNd8       "hhd"
#endif
#ifndef SCNd16
#  define SCNd16      "hd"
#endif
#ifndef SCNd32
#  ifdef __LP64__
#    define SCNd32    "d"
#  else
#    define SCNd32    "ld"
#  endif
#endif
#ifndef SCNd64
#  define SCNd64      "lld"
#endif

#ifndef SCNi8
#  define SCNi8       "hhi"
#endif
#ifndef SCNi16
#  define SCNi16      "hi"
#endif
#ifndef SCNi32
#  ifdef __LP64__
#    define SCNi32    "i"
#  else
#    define SCNi32    "li"
#  endif
#endif
#ifndef SCNi64
#  define SCNi64      "lli"
#endif

/* Scan format macros for unsigned integers */
#ifndef SCNu8
#  define SCNu8       "hhu"
#endif
#ifndef SCNu16
#  define SCNu16      "hu"
#endif
#ifndef SCNu32
#  ifdef __LP64__
#    define SCNu32    "u"
#  else
#    define SCNu32    "lu"
#  endif
#endif
#ifndef SCNu64
#  define SCNu64      "llu"
#endif

#ifndef SCNo8
#  define SCNo8       "hho"
#endif
#ifndef SCNo16
#  define SCNo16      "ho"
#endif
#ifndef SCNo32
#  ifdef __LP64__
#    define SCNo32    "o"
#  else
#    define SCNo32    "lo"
#  endif
#endif
#ifndef SCNo64
#  define SCNo64      "llo"
#endif

#ifndef SCNx8
#  define SCNx8       "hhx"
#endif
#ifndef SCNx16
#  define SCNx16      "hx"
#endif
#ifndef SCNx32
#  ifdef __LP64__
#    define SCNx32    "x"
#  else
#    define SCNx32    "lx"
#  endif
#endif
#ifndef SCNx64
#  define SCNx64      "llx"
#endif

#endif /* _INTTYPES_H */