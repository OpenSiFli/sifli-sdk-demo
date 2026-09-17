/* SPDX-License-Identifier: Apache-2.0 */
#include <bf0_hal.h>
#include <stdint.h>

/* Override the SDK weak implementation for this application. Invalidating the
 * whole cache after an EPIC transfer would also discard dirty JPEG/image data
 * written by the media thread while EPIC was running. Preserve those unrelated
 * lines by invalidating only the requested range (rounded by CMSIS to lines).
 * Returns 0 for range/no-op maintenance, or -1 for an overflowing address range. */
int mpu_dcache_invalidate(void *data, uint32_t size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    uintptr_t address = (uintptr_t)data;
    const uint32_t chunk_limit = (uint32_t)INT32_MAX - (__SCB_DCACHE_LINE_SIZE - 1U);

    if (size == 0U || !IS_DCACHED_RAM(data))
        return 0;
    /* CMSIS uses 32-bit addresses. Check the last included byte without
     * forming an overflowing end pointer, before invalidating any line. */
    if (address > UINT32_MAX || size - 1U > UINT32_MAX - address)
        return -1;

    while (size != 0U)
    {
        uint32_t chunk = size > chunk_limit ? chunk_limit : size;

        /* CMSIS adds the initial line offset to its signed byte count. */
        SCB_InvalidateDCache_by_Addr((void *)address, (int32_t)chunk);
        size -= chunk;
        if (size != 0U)
            address += chunk;
    }
#else
    (void)data;
    (void)size;
#endif
    return 0;
}
