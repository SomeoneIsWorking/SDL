/* Fence-set completion regression; exercises the shipping wait policy. */
#include "../src/gpu/webgpu/SDL_gpu_webgpu_fence.h"
#include <SDL3/SDL_main.h>
#include <stdio.h>

typedef struct FenceTimeline
{
    Uint32 pass;
    Uint32 queries;
    Uint32 ready[2];
} FenceTimeline;

static bool query(void *context, Uint32 index)
{
    FenceTimeline *timeline = context;
    ++timeline->queries;
    return timeline->pass >= timeline->ready[index];
}

static void yield(void *context)
{
    ++((FenceTimeline *)context)->pass;
}

static int check(bool wait_all, Uint32 count, Uint32 first, Uint32 second,
                 Uint32 expected_pass, Uint32 expected_queries)
{
    FenceTimeline timeline = { 0, 0, { first, second } };
    WEBGPU_WaitForFenceSet(&timeline, wait_all, count, query, yield);
    printf("wait_all=%d fences=%u: pass=%u queries=%u (expected %u/%u)\n",
           wait_all, count, timeline.pass, timeline.queries, expected_pass, expected_queries);
    return timeline.pass != expected_pass || timeline.queries != expected_queries;
}

int main(int argc, char **argv)
{
    int failures = 0;
    (void)argc;
    (void)argv;
    failures += check(true, 2, 0, 2, 2, 6);
    failures += check(true, 2, 2, 0, 2, 6);
    failures += check(true, 2, 2, 2, 2, 6);
    failures += check(false, 2, 0, 2, 0, 2);
    failures += check(false, 2, 2, 1, 1, 4);
    failures += check(true, 0, 0, 0, 0, 0);
    failures += check(false, 0, 0, 0, 0, 0);
    printf("WebGPU fence wait: 7 cases, %d failures\n", failures);
    return failures ? 1 : 0;
}
