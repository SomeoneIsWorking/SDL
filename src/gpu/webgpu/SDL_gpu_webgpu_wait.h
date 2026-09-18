/* Private WebGPU wait policy: how long this backend waits, and for what. */
#ifndef SDL_gpu_webgpu_wait_h_
#define SDL_gpu_webgpu_wait_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

/* Two things in this backend waited on the browser without a bound: a fence
 * set, and a swapchain texture. A condition that will never be satisfied is
 * indistinguishable from slow work when the wait has no bound, and neither of
 * those waits printed anything at all. A lost device, a submission that never
 * reached the queue, and a thread the completion callback cannot be delivered
 * to all produce exactly that, and all three are silent. Measured: 117% of a
 * core for 17 hours on a GPU selftest that printed its banner and then
 * nothing.
 *
 * So both waits are bounded by this one deadline and both report what they
 * were waiting for. The bound is not a tuning knob and it is not there to make
 * a slow machine pass -- real GPU work resolves in milliseconds, so ten
 * seconds is the line past which the browser is not slow but broken, and
 * saying so is the only way the caller can tell anyone. */
#define SDL_WEBGPU_WAIT_TIMEOUT_NS (10 * SDL_NS_PER_SECOND)

/* How many futures one timed WaitAny may cover. This backend waits on one at a
 * time, but the instance limit is declared once at creation and cannot be
 * raised later, so it is set to the implementation's own default rather than
 * to the single future in use today. */
#define WEBGPU_TIMED_WAIT_ANY_MAX_COUNT 64

/* The longest one pass of a fence wait may block before the caller re-checks
 * its own deadline. It is not how long a fence takes: a timed WaitAny returns
 * the moment the future completes, so this only bounds how late the enclosing
 * timeout can be noticed. */
#define WEBGPU_FENCE_WAIT_SLICE_NS (SDL_NS_PER_SECOND / 2)

/* Query every fence in the same pass: completed fences must not be counted
 * repeatedly toward wait_all while another fence remains pending. Yield lets
 * the host deliver asynchronous completion callbacks between passes.
 *
 * Returns true when the set was satisfied. On false the wait timed out and
 * `*completed_out` holds how many of `count` had resolved -- the denominator
 * the caller needs to say whether NOTHING arrived or only the last one is
 * missing, which point at different causes. */
static inline bool WEBGPU_WaitForFenceSet(void *context, bool wait_all, Uint32 count,
                                          bool (*query)(void *, Uint32), void (*yield)(void *),
                                          Uint32 *completed_out)
{
    const Uint64 deadline = SDL_GetTicksNS() + SDL_WEBGPU_WAIT_TIMEOUT_NS;

    if (completed_out != NULL) {
        *completed_out = 0;
    }
    if (count == 0) {
        return true;
    }
    for (;;) {
        Uint32 completed = 0;
        for (Uint32 i = 0; i < count; ++i) {
            if (query(context, i)) {
                ++completed;
            }
        }
        if (completed_out != NULL) {
            *completed_out = completed;
        }
        if (wait_all ? completed == count : completed != 0) {
            return true;
        }
        if (SDL_GetTicksNS() >= deadline) {
            return false;
        }
        yield(context);
    }
}
#endif
