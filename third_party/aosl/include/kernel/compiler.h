/***************************************************************************
 * Module:	compile
 *
 * Copyright © 2025 Agora
 * This file is part of AOSL, an open source project.
 * Licensed under the Apache License, Version 2.0, with certain conditions.
 * Refer to the "LICENSE" file in the root directory for more information.
 ***************************************************************************/
#ifndef __KERNEL_COMPILER_H__
#define __KERNEL_COMPILER_H__

#ifndef likely
#define likely(x) (x)
#endif

#ifndef unlikely
#define unlikely(x) (x)
#endif

#ifndef FuncReturnAddress
/*
 * ARM Compiler 5 (ARMCC) defines __ARMCC_VERSION and supports __return_address().
 * ARM Compiler 6 (armclang) also defines __ARMCC_VERSION but does NOT support
 * __return_address() — it defines __clang__ so we can differentiate.
 * For armclang, GCC, and other compilers, use __builtin_return_address(0).
 */
#if defined(__ARMCC_VERSION) && !defined(__clang__)
#define FuncReturnAddress() __return_address()
#elif defined(_MSC_VER)
#include <intrin.h>
#define FuncReturnAddress() _ReturnAddress()
#else
#define FuncReturnAddress() __builtin_return_address(0)
#endif
#endif

#endif /* __KERNEL_COMPILER_H__ */
