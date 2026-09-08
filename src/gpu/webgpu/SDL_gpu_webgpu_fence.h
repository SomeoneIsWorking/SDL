/* Private WebGPU fence-set wait policy. */
#ifndef SDL_gpu_webgpu_fence_h_
#define SDL_gpu_webgpu_fence_h_

#include <SDL3/SDL_stdinc.h>

/* Query every fence in the same pass: completed fences must not be counted
 * repeatedly toward wait_all while another fence remains pending. Yield lets
 * the host deliver asynchronous completion callbacks between passes. */
static inline void WEBGPU_WaitForFenceSet(void *context, bool wait_all, Uint32 count,
                                          bool (*query)(void *, Uint32), void (*yield)(void *))
{
    if (count == 0) {
        return;
    }
    for (;;) {
        Uint32 completed = 0;
        for (Uint32 i = 0; i < count; ++i) {
            if (query(context, i)) {
                ++completed;
            }
        }
        if (wait_all ? completed == count : completed != 0) {
            return;
        }
        yield(context);
    }
}
#endif
