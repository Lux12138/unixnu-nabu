/* SPDX-License-Identifier: MIT */
#include <kern/startup.h>
extern void native_adreno_a6xx_start(void);
static void nabu_gpu_start(const void *argument)
{
    (void)argument;
    native_adreno_a6xx_start();
}
STARTUP_ARG(EARLY_BOOT,STARTUP_RANK_LAST,nabu_gpu_start,0);
