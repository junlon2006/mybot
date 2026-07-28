#include <hal/aosl_hal_utils.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <asm/sfc_norflash_api.h> // For get_norflash_uuid

int aosl_hal_get_uuid (char buf [], int buf_sz)
{
    // Generate a RFC4122-like random UUID
    // xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    uint32_t r0 = rand32();
    uint32_t r1 = rand32();
    uint32_t r2 = rand32();
    uint32_t r3 = rand32();

    // Set version 4 (random) and variant (10xx)
    r1 = (r1 & 0xFFFF0FFF) | 0x00004000;
    r2 = (r2 & 0x3FFFFFFF) | 0x80000000;

    // Use a fixed format that ensures we meet the 32-char minimum if buffer allows.
    // The test expects EXACTLY 32 hex chars, NO separators like '-'.
    // If buffer is too small, snprintf correctly truncates it.
    snprintf(buf, buf_sz,
        "%08x%08x%08x%08x",
        (unsigned int)r0, (unsigned int)r1, (unsigned int)r2, (unsigned int)r3
    );

	return 0;
}

int aosl_hal_os_version (char buf [], int buf_sz)
{
	if (buf_sz <= 1) return -1;

    snprintf(buf, buf_sz, "JieLi Pi32v2 AC79 SDK");
	return 0;
}
