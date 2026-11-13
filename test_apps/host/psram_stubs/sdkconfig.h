#pragma once

/* A disabled Kconfig boolean is undefined, rather than defined to zero. */
#ifndef CFT_TEST_NO_PSRAM
#define CONFIG_SPIRAM 1
#ifndef CFT_TEST_ALLOC_ONLY
#define CONFIG_CF_TUNNEL_PSRAM_FALLBACK 1
#endif
#endif
