/* One retained offscreen texture, created ONCE and reused -- not recreated --
   across many separated composite-and-download cycles, with ordinary frames
   composited straight to the swapchain in between.

   The reuse is the variable. A backend may keep per-texture state that is
   correct the first time a texture is a blit destination and stale on the
   twentieth, and both the one-shot offscreen composite and the sustained
   swapchain loop miss that: the first never repeats, the second gets a fresh
   destination object every frame. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#define SCENE_W 4u
#define SCENE_H 2u
#define OUT_SIZE 64u
#define TOTAL_FRAMES 200u
#define REQUEST_EVERY 5u /* mirrors present_luma's "every" cadence */

static SDL_Window *window;
static SDL_GPUDevice *device;
static SDL_GPUTexture *scene;
static SDL_GPUTexture *retained; /* created ONCE, reused every request --
                                     mirrors a caching capture target's
                                     g_capture_texture caching. */
static unsigned frame;
static unsigned requests;
static unsigned last_nonblack, last_total;

static void fail(const char *what) {
    printf("[reuse-probe] %s FAILED: %s\n", what, SDL_GetError());
    emscripten_cancel_main_loop();
}

static void tick(void) {
    SDL_GPUCommandBuffer *cb;
    SDL_GPUTexture *swap = NULL;
    Uint32 w = 0, h = 0;
    SDL_GPUBlitInfo blit;
    int this_is_request;

    frame++;
    if (frame > TOTAL_FRAMES) {
        printf("[reuse-probe] COMPLETED %u frames, %u retained-texture request(s); "
               "LAST request: %u/%u nonblack pixel(s)\n",
               TOTAL_FRAMES, requests, last_nonblack, last_total);
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        emscripten_cancel_main_loop();
        return;
    }

    cb = SDL_AcquireGPUCommandBuffer(device);
    if (!cb) { fail("acquire command buffer"); return; }
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cb, window, &swap, &w, &h)) {
        fail("acquire swapchain");
        return;
    }
    if (!swap) {
        SDL_CancelGPUCommandBuffer(cb);
        return;
    }

    this_is_request = (frame % REQUEST_EVERY) == 0;

    if (this_is_request) {
        /* gpu_present_composite(): scene -> retained (the CACHED, REUSED
           texture -- not a fresh one), clear load op, linear filter. */
        memset(&blit, 0, sizeof blit);
        blit.source.texture = scene;
        blit.source.w = SCENE_W;
        blit.source.h = SCENE_H;
        blit.destination.texture = retained;
        blit.destination.w = OUT_SIZE;
        blit.destination.h = OUT_SIZE;
        blit.load_op = SDL_GPU_LOADOP_CLEAR;
        blit.clear_color.a = 1.0f;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cb, &blit);

        /* The capture step: retained -> swap (output != rendered),
           DONT_CARE load op. */
        memset(&blit, 0, sizeof blit);
        blit.source.texture = retained;
        blit.source.w = OUT_SIZE;
        blit.source.h = OUT_SIZE;
        blit.destination.texture = swap;
        blit.destination.w = w;
        blit.destination.h = h;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cb, &blit);

        /* download FROM retained, same command buffer, same order as
           the capture step. */
        {
            SDL_GPUTransferBufferCreateInfo tinfo;
            SDL_GPUCopyPass *copy;
            SDL_GPUTextureRegion region;
            SDL_GPUTextureTransferInfo dst;
            SDL_GPUTransferBuffer *download;
            SDL_GPUFence *fence;
            void *mapped;

            memset(&tinfo, 0, sizeof tinfo);
            tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
            tinfo.size = OUT_SIZE * OUT_SIZE * 4u;
            download = SDL_CreateGPUTransferBuffer(device, &tinfo);
            copy = SDL_BeginGPUCopyPass(cb);
            memset(&region, 0, sizeof region);
            memset(&dst, 0, sizeof dst);
            region.texture = retained;
            region.w = OUT_SIZE;
            region.h = OUT_SIZE;
            region.d = 1;
            dst.transfer_buffer = download;
            SDL_DownloadFromGPUTexture(copy, &region, &dst);
            SDL_EndGPUCopyPass(copy);

            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
            if (!fence) { fail("submit"); return; }
            SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);

            mapped = SDL_MapGPUTransferBuffer(device, download, false);
            if (mapped) {
                const unsigned char *px = (const unsigned char *)mapped;
                unsigned nonblack = 0, i, n = OUT_SIZE * OUT_SIZE;
                for (i = 0; i < n; i++)
                    if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2])
                        nonblack++;
                requests++;
                last_nonblack = nonblack;
                last_total = n;
                printf("[reuse-probe] frame %u (request #%u): %u/%u nonblack pixel(s) "
                       "in the REUSED retained texture\n",
                       frame, requests, nonblack, n);
                SDL_UnmapGPUTransferBuffer(device, download);
            } else {
                printf("[reuse-probe] map FAILED: %s\n", SDL_GetError());
            }
            SDL_ReleaseGPUTransferBuffer(device, download);
        }
        return;
    }

    /* Ordinary frame: composite straight to the swapchain, no retained
       texture involved -- matches gpu_present_composite's other path. */
    memset(&blit, 0, sizeof blit);
    blit.source.texture = scene;
    blit.source.w = SCENE_W;
    blit.source.h = SCENE_H;
    blit.destination.texture = swap;
    blit.destination.w = w;
    blit.destination.h = h;
    blit.load_op = SDL_GPU_LOADOP_CLEAR;
    blit.clear_color.a = 1.0f;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cb, &blit);
    if (!SDL_SubmitGPUCommandBuffer(cb)) { fail("submit"); return; }
}

int main(int argc, char **argv) {
    static const uint32_t pixels[SCENE_W * SCENE_H] = {
        0xffff0000u, 0xffff0000u, 0xff00ff00u, 0xff00ff00u,
        0xff0000ffu, 0xff0000ffu, 0xffffffffu, 0xffffffffu,
    };
    SDL_GPUTextureCreateInfo info;
    SDL_GPUTransferBufferCreateInfo tinfo;
    SDL_GPUTransferBuffer *upload;
    SDL_GPUCommandBuffer *cb;
    SDL_GPUCopyPass *copy;
    void *mapped;
    (void)argc; (void)argv;

    puts("[reuse-probe] init");
    if (!SDL_Init(SDL_INIT_VIDEO)) { fail("SDL_Init"); return 1; }
    window = SDL_CreateWindow("retained reuse", 64, 64, 0);
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_WGSL, false, "webgpu");
    if (!window || !device || !SDL_ClaimWindowForGPUDevice(device, window)) {
        fail("window/device/claim");
        return 1;
    }
    printf("[reuse-probe] backend=%s\n", SDL_GetGPUDeviceDriver(device));

    memset(&info, 0, sizeof info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = SCENE_W;
    info.height = SCENE_H;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    scene = SDL_CreateGPUTexture(device, &info);

    info.width = OUT_SIZE;
    info.height = OUT_SIZE;
    retained = SDL_CreateGPUTexture(device, &info);
    if (!scene || !retained) { fail("texture creation"); return 1; }

    memset(&tinfo, 0, sizeof tinfo);
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = sizeof pixels;
    upload = SDL_CreateGPUTransferBuffer(device, &tinfo);
    mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    memcpy(mapped, pixels, sizeof pixels);
    SDL_UnmapGPUTransferBuffer(device, upload);
    cb = SDL_AcquireGPUCommandBuffer(device);
    copy = SDL_BeginGPUCopyPass(cb);
    {
        SDL_GPUTextureTransferInfo src;
        SDL_GPUTextureRegion dstreg;
        memset(&src, 0, sizeof src);
        memset(&dstreg, 0, sizeof dstreg);
        src.transfer_buffer = upload;
        dstreg.texture = scene;
        dstreg.w = SCENE_W;
        dstreg.h = SCENE_H;
        dstreg.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dstreg, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_ReleaseGPUTransferBuffer(device, upload);

    printf("[reuse-probe] running %u frames, a retained-texture request every %u frame(s)\n",
           TOTAL_FRAMES, REQUEST_EVERY);
    emscripten_set_main_loop(tick, 0, 0);
    return 0;
}
