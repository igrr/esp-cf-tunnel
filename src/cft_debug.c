#include <stdio.h>

#include "cft_debug.h"

#ifdef CFT_DEBUG_STATS
cft_debug_t cft_debug;

int cft_debug_stats(char *buf, size_t size)
{
    return snprintf(buf, size,
                    ",\"h2_bytes\":%u,\"h2_peak\":%u,\"sq_bytes\":%u,\"sq_peak\":%u,\"cft_streams\":%u,\"held\":%u,\"send_window_min\":%ld,\"send_window_stalls\":%u",
                    (unsigned)cft_debug.h2_bytes, (unsigned)cft_debug.h2_peak, (unsigned)cft_debug.sq_bytes,
                    (unsigned)cft_debug.sq_peak, (unsigned)cft_debug.streams, (unsigned)cft_debug.held,
                    cft_debug.send_window_min, cft_debug.send_window_stalls);
}
#endif
