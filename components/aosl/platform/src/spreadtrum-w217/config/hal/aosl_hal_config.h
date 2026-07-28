#ifndef __AOSL_HAL_CONFIG_H__
#define __AOSL_HAL_CONFIG_H__

/*
 * RVCT (ARM Compiler 5) compatibility
 * AOSL uses GCC extensions (anonymous unions) that RVCT
 * does not support by default. Enable them here for this platform.
 */
#pragma anon_unions

/*
 * ARM little-endian identification for AOSL byteorder detection.
 * The AOSL kernel/byteorder/generic.h checks __ARMEL__ at compile time
 * to select the correct byteorder header.
 */
#ifndef __ARMEL__
#define __ARMEL__
#endif

#define AOSL_HAL_HAVE_EPOLL 0
#define AOSL_HAL_HAVE_POLL 0
#define AOSL_HAL_HAVE_SELECT 1

#define AOSL_HAL_HAVE_COND 0
#define AOSL_HAL_HAVE_SEM 1

#define AOSL_HAL_HAVE_HWRNG 0

#endif /* __AOSL_HAL_CONFIG_H__ */
