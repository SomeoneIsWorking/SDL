/* SUSTAINED compositing into the real window swapchain: acquire the swapchain
   texture with SDL_WaitAndAcquireGPUSwapchainTexture and blit a known
   non-black scene into it every frame, for many frames, then on a chosen LATE
   frame download that exact swapchain texture in the same command buffer (a
   copy pass right after the blit) and check that what the blit wrote is there.

   A one-shot offscreen composite exercises none of this: the swapchain texture
   is a different object every frame, and a backend that loses the blit only
   after the swapchain has been reacquired a few dozen times passes every
   single-frame test there is. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#define SCENE_W 4u
#define SCENE_H 2u
#define CHECK_AT_FRAME 30
#define TOTAL_FRAMES 40

static SDL_Window *window;
static SDL_GPUDevice *device;
static SDL_GPUTexture *scene;
static unsigned frame;
static SDL_GPUTransferBuffer *check_download;
static Uint32 check_w, check_h;

static void fail(const char *what) {
    printf("[sustained-probe] %s FAILED: %s\n", what, SDL_GetError());
    emscripten_cancel_main_loop();
}

static void tick(void) {
    SDL_GPUCommandBuffer *cb;
    SDL_GPUTexture *swap = NULL;
    Uint32 w = 0, h = 0;
    SDL_GPUBlitInfo blit;

    frame++;
    if (frame > TOTAL_FRAMES) {
        puts("[sustained-probe] COMPLETED all frames without incident");
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
        printf("[sustained-probe] frame %u: no swapchain image this tick (minimised/in-flight)\n", frame);
        SDL_CancelGPUCommandBuffer(cb);
        return;
    }

    /* Exactly gpu_present_composite(): aspect-fit-ish blit, clear load op,
       linear filter, scene -> the real acquired swapchain texture. */
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

    if (frame == CHECK_AT_FRAME) {
        /* Download DIRECTLY from the just-composited swapchain texture, in
           the SAME command buffer, via a copy pass (COPY_SRC, not sampling
           -- unlike a blit source this needs no TextureBinding usage, and
           this is the one thing the title's own capture path never tries:
           a capture would refuse (rendered == output) rather than
           reading a swapchain texture directly). This is exactly "did the
           blit two lines above actually deposit content into the real
           swapchain image", nothing more. */
        SDL_GPUTransferBufferCreateInfo tinfo;
        SDL_GPUCopyPass *copy;
        SDL_GPUTextureRegion region;
        SDL_GPUTextureTransferInfo dst;
        memset(&tinfo, 0, sizeof tinfo);
        tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tinfo.size = (Uint32)w * h * 4u;
        check_download = SDL_CreateGPUTransferBuffer(device, &tinfo);
        copy = SDL_BeginGPUCopyPass(cb);
        memset(&region, 0, sizeof region);
        memset(&dst, 0, sizeof dst);
        region.texture = swap;
        region.w = w;
        region.h = h;
        region.d = 1;
        dst.transfer_buffer = check_download;
        SDL_DownloadFromGPUTexture(copy, &region, &dst);
        SDL_EndGPUCopyPass(copy);
        check_w = w;
        check_h = h;
        printf("[sustained-probe] frame %u: composited into swapchain %ux%u AND queued a same-buffer direct download\n", frame, w, h);
    }

    if (!SDL_SubmitGPUCommandBuffer(cb)) {
        fail("submit");
        return;
    }

    if (frame == CHECK_AT_FRAME && check_download) {
        /* The transfer buffer's contents are ready once THIS command buffer's
           work completes; wait for GPU idle (this probe has no fence handle
           for a plain SDL_SubmitGPUCommandBuffer) before mapping. */
        SDL_WaitForGPUIdle(device);
        void *mapped = SDL_MapGPUTransferBuffer(device, check_download, false);
        if (mapped) {
            const unsigned char *px = (const unsigned char *)mapped;
            unsigned nonblack = 0, i, n = check_w * check_h;
            for (i = 0; i < n; i++)
                if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2])
                    nonblack++;
            printf("[sustained-probe] RESULT at frame %u: %u/%u nonblack pixel(s) read DIRECTLY from a SUSTAINED-loop swapchain composite\n",
                   frame, nonblack, n);
            SDL_UnmapGPUTransferBuffer(device, check_download);
        } else {
            printf("[sustained-probe] map FAILED: %s\n", SDL_GetError());
        }
        SDL_ReleaseGPUTransferBuffer(device, check_download);
        check_download = NULL;
    }
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

    puts("[sustained-probe] init");
    if (!SDL_Init(SDL_INIT_VIDEO)) { fail("SDL_Init"); return 1; }
    window = SDL_CreateWindow("sustained composite", 64, 64, 0);
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_WGSL, false, "webgpu");
    if (!window || !device || !SDL_ClaimWindowForGPUDevice(device, window)) {
        fail("window/device/claim");
        return 1;
    }
    printf("[sustained-probe] backend=%s\n", SDL_GetGPUDeviceDriver(device));

    memset(&info, 0, sizeof info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = SCENE_W;
    info.height = SCENE_H;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    scene = SDL_CreateGPUTexture(device, &info);

    if (!scene) { fail("texture creation"); return 1; }

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

    printf("[sustained-probe] running %d frames, checking at frame %d\n", TOTAL_FRAMES, CHECK_AT_FRAME);
    emscripten_set_main_loop(tick, 0, 0);
    return 0;
}
