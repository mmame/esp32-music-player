/* cpu_detect_stub.cpp
 * Non-x86 stub for SoundTouch CPU-extension detection.
 * On Xtensa/ESP32 there are no MMX/SSE extensions; return 0.
 */
#include "cpu_detect.h"

uint detectCPUextensions(void)
{
    return 0;   /* no x86 SIMD extensions on Xtensa */
}

void disableExtensions(uint /*wDisableMask*/)
{
    /* no-op on Xtensa */
}
