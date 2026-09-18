/* A composite built from three SEPARATE command-buffer submissions, each with
   its own SDL_SubmitGPUCommandBufferAndAcquireFence + SDL_WaitForGPUFences:
   upload a scene texture, then blit it aspect-fit into an offscreen output and
   copy that output to a download buffer, then read the buffer back. Headless,
   no window.

   The point of the split is that WebGPU's queue completion is asynchronous:
   work each submission's fence has reported done must be visible to the next
   submission, and the bytes read in the third must be the ones the second
   blitted. This test also counts the queue's submits and completion
   registrations from JS, so a backend that batches or drops a submission is
   caught by the count and not only by the pixels. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#define SCENE_W 4u
#define SCENE_H 2u
#define OUT_SIZE 12u

EM_JS(void, instrument_queue, (), {
    const submit = GPUQueue.prototype.submit;
    const fence = GPUQueue.prototype.onSubmittedWorkDone;
    globalThis.qEvents = [];
    GPUQueue.prototype.submit = function(...args) {
        console.log("[composite-probe] GPUQueue.submit() called, " + args[0].length + " command buffer(s)");
        qEvents.push("submit");
        return submit.apply(this, args);
    };
    GPUQueue.prototype.onSubmittedWorkDone = function(...args) {
        console.log("[composite-probe] GPUQueue.onSubmittedWorkDone() requested");
        qEvents.push("fence-requested");
        const p = fence.apply(this, args);
        p.then(() => { console.log("[composite-probe] onSubmittedWorkDone PROMISE RESOLVED"); qEvents.push("fence-resolved"); },
               (e) => { console.log("[composite-probe] onSubmittedWorkDone PROMISE REJECTED: " + e); qEvents.push("fence-rejected"); });
        return p;
    };
});
EM_JS(int, queue_event_count, (), { return qEvents.length; });

static SDL_GPUDevice *device;

static int submit_and_wait(const char *label, SDL_GPUCommandBuffer *cb) {
    SDL_GPUFence *fence;
    int waited;

    printf("[composite-probe] %s: submitting...\n", label);
    fflush(stdout);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    if (!fence) {
        printf("[composite-probe] %s: submit FAILED: %s\n", label, SDL_GetError());
        return 0;
    }
    printf("[composite-probe] %s: submitted, waiting for fence...\n", label);
    fflush(stdout);
    waited = SDL_WaitForGPUFences(device, true, &fence, 1);
    printf("[composite-probe] %s: wait returned %d (%d queue event(s) so far)\n",
           label, waited, queue_event_count());
    fflush(stdout);
    SDL_ReleaseGPUFence(device, fence);
    return waited;
}

static SDL_GPUTexture *make_texture(uint32_t w, uint32_t h, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info;
    memset(&info, 0, sizeof info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    info.usage = usage;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    return SDL_CreateGPUTexture(device, &info);
}

int main(int argc, char **argv) {
    SDL_GPUTexture *scene, *capture_target, *output;
    SDL_GPUTransferBuffer *upload, *download;
    SDL_GPUCommandBuffer *cb;
    SDL_GPUCopyPass *copy;
    SDL_GPUBlitInfo blit;
    SDL_GPUTransferBufferCreateInfo tinfo;
    SDL_GPUTextureTransferInfo dst;
    SDL_GPUTextureRegion region;
    void *mapped;
    static const uint32_t pixels[SCENE_W * SCENE_H] = {
        0xffff0000u, 0xffff0000u, 0xff00ff00u, 0xff00ff00u,
        0xff0000ffu, 0xff0000ffu, 0xffffffffu, 0xffffffffu,
    };
    (void)argc; (void)argv;

    instrument_queue();
    puts("[composite-probe] init");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("[composite-probe] SDL_Init FAILED: %s\n", SDL_GetError());
        return 1;
    }
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_WGSL, false, "webgpu");
    if (!device) {
        printf("[composite-probe] device FAILED: %s\n", SDL_GetError());
        return 1;
    }
    printf("[composite-probe] backend=%s\n", SDL_GetGPUDeviceDriver(device));

    /* A depth binding created immediately after
       device creation in the real title and before the selftest banner's
       next log line ("gpu: GPU device created") -- that next line never
       appears in the browser, so this step is a suspect. */
    {
        static const SDL_GPUTextureFormat depth_formats[] = {
            SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D16_UNORM,
            SDL_GPU_TEXTUREFORMAT_D24_UNORM,
        };
        SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        const SDL_GPUTextureUsageFlags depth_usage =
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        unsigned i;
        for (i = 0; i < SDL_arraysize(depth_formats); i++) {
            printf("[composite-probe] depth: checking format %d support...\n", depth_formats[i]);
            fflush(stdout);
            if (SDL_GPUTextureSupportsFormat(device, depth_formats[i], SDL_GPU_TEXTURETYPE_2D, depth_usage)) {
                depth_format = depth_formats[i];
                printf("[composite-probe] depth: format %d supported\n", depth_formats[i]);
                break;
            }
        }
        if (depth_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
            printf("[composite-probe] depth: NO sampleable depth format found: %s\n", SDL_GetError());
        } else {
            SDL_GPUTextureCreateInfo dinfo;
            SDL_GPUSamplerCreateInfo sinfo;
            SDL_GPUTexture *depth_tex;
            SDL_GPUSampler *depth_sampler;
            memset(&dinfo, 0, sizeof dinfo);
            dinfo.type = SDL_GPU_TEXTURETYPE_2D;
            dinfo.format = depth_format;
            dinfo.usage = depth_usage;
            dinfo.width = dinfo.height = dinfo.layer_count_or_depth = dinfo.num_levels = 1;
            depth_tex = SDL_CreateGPUTexture(device, &dinfo);
            memset(&sinfo, 0, sizeof sinfo);
            sinfo.min_filter = sinfo.mag_filter = SDL_GPU_FILTER_NEAREST;
            sinfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            sinfo.address_mode_u = sinfo.address_mode_v = sinfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            depth_sampler = SDL_CreateGPUSampler(device, &sinfo);
            printf("[composite-probe] depth: texture=%p sampler=%p\n", (void *)depth_tex, (void *)depth_sampler);
            if (depth_tex && depth_sampler) {
                SDL_GPUCommandBuffer *dcb = SDL_AcquireGPUCommandBuffer(device);
                SDL_GPUDepthStencilTargetInfo dtarget;
                SDL_GPURenderPass *dpass;
                memset(&dtarget, 0, sizeof dtarget);
                dtarget.texture = depth_tex;
                dtarget.clear_depth = 1.0f;
                dtarget.load_op = SDL_GPU_LOADOP_CLEAR;
                dtarget.store_op = SDL_GPU_STOREOP_STORE;
                dtarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
                dtarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
                printf("[composite-probe] depth: beginning depth-only render pass...\n");
                fflush(stdout);
                dpass = SDL_BeginGPURenderPass(dcb, NULL, 0, &dtarget);
                printf("[composite-probe] depth: pass=%p\n", (void *)dpass);
                if (dpass) {
                    SDL_EndGPURenderPass(dpass);
                    printf("[composite-probe] depth: submitting plain SDL_SubmitGPUCommandBuffer...\n");
                    fflush(stdout);
                    printf("[composite-probe] depth: submit ok=%d\n", (int)SDL_SubmitGPUCommandBuffer(dcb));
                }
            }
        }
    }

    scene = make_texture(SCENE_W, SCENE_H, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    capture_target = make_texture(OUT_SIZE, OUT_SIZE, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    output = make_texture(OUT_SIZE, OUT_SIZE, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    if (!scene || !capture_target || !output) {
        printf("[composite-probe] texture creation FAILED: %s\n", SDL_GetError());
        return 1;
    }

    /* Step 1: upload the scene pixels (mirrors upload_scene). */
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
    if (!submit_and_wait("upload", cb)) {
        puts("[composite-probe] STOPPED at upload");
        return 1;
    }
    SDL_ReleaseGPUTransferBuffer(device, upload);

    /* Step 2: composite blit scene->capture_target (gpu_present_composite),
       then blit the capture target's output (the
       output copy), then download FROM capture_target -- same command
       buffer, same order as the title. */
    memset(&tinfo, 0, sizeof tinfo);
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tinfo.size = OUT_SIZE * OUT_SIZE * 4u;
    download = SDL_CreateGPUTransferBuffer(device, &tinfo);
    cb = SDL_AcquireGPUCommandBuffer(device);

    memset(&blit, 0, sizeof blit);
    blit.source.texture = scene;
    blit.source.w = SCENE_W;
    blit.source.h = SCENE_H;
    blit.destination.texture = capture_target;
    blit.destination.x = 2; blit.destination.y = 1;
    blit.destination.w = 8; blit.destination.h = 4;
    blit.load_op = SDL_GPU_LOADOP_CLEAR;
    blit.clear_color.a = 1.0f;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cb, &blit);
    printf("[composite-probe] composite blit recorded\n");

    memset(&blit, 0, sizeof blit);
    blit.source.texture = capture_target;
    blit.source.w = OUT_SIZE;
    blit.source.h = OUT_SIZE;
    blit.destination.texture = output;
    blit.destination.w = OUT_SIZE;
    blit.destination.h = OUT_SIZE;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cb, &blit);
    printf("[composite-probe] output blit recorded\n");

    copy = SDL_BeginGPUCopyPass(cb);
    memset(&region, 0, sizeof region);
    memset(&dst, 0, sizeof dst);
    region.texture = capture_target;
    region.w = OUT_SIZE;
    region.h = OUT_SIZE;
    region.d = 1;
    dst.transfer_buffer = download;
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);
    printf("[composite-probe] download recorded\n");

    if (!submit_and_wait("composite+download", cb)) {
        puts("[composite-probe] STOPPED at composite+download");
        return 1;
    }

    mapped = SDL_MapGPUTransferBuffer(device, download, false);
    if (!mapped) {
        printf("[composite-probe] map FAILED: %s\n", SDL_GetError());
        return 1;
    }
    {
        const unsigned char *px = (const unsigned char *)mapped;
        unsigned nonblack = 0, i;
        for (i = 0; i < OUT_SIZE * OUT_SIZE; i++) {
            if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2])
                nonblack++;
        }
        printf("[composite-probe] RESULT: %u/%u nonblack pixel(s) in the composited capture texture\n",
               nonblack, OUT_SIZE * OUT_SIZE);
    }
    SDL_UnmapGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, download);

    /* Step 3: a third, independent submission reading the swapchain-shaped
       output texture -- mirrors read_output(), the selftest's final call. */
    {
        SDL_GPUTransferBufferCreateInfo tinfo2;
        SDL_GPUTransferBuffer *download2;
        memset(&tinfo2, 0, sizeof tinfo2);
        tinfo2.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tinfo2.size = OUT_SIZE * OUT_SIZE * 4u;
        download2 = SDL_CreateGPUTransferBuffer(device, &tinfo2);
        cb = SDL_AcquireGPUCommandBuffer(device);
        copy = SDL_BeginGPUCopyPass(cb);
        memset(&region, 0, sizeof region);
        memset(&dst, 0, sizeof dst);
        region.texture = output;
        region.w = OUT_SIZE;
        region.h = OUT_SIZE;
        region.d = 1;
        dst.transfer_buffer = download2;
        SDL_DownloadFromGPUTexture(copy, &region, &dst);
        SDL_EndGPUCopyPass(copy);
        if (!submit_and_wait("read-output", cb)) {
            puts("[composite-probe] STOPPED at read-output");
            return 1;
        }
        mapped = SDL_MapGPUTransferBuffer(device, download2, false);
        if (mapped) {
            const unsigned char *px = (const unsigned char *)mapped;
            unsigned nonblack = 0, i;
            for (i = 0; i < OUT_SIZE * OUT_SIZE; i++) {
                if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2])
                    nonblack++;
            }
            printf("[composite-probe] RESULT: %u/%u nonblack pixel(s) in the output texture\n",
                   nonblack, OUT_SIZE * OUT_SIZE);
            SDL_UnmapGPUTransferBuffer(device, download2);
        } else {
            printf("[composite-probe] output map FAILED: %s\n", SDL_GetError());
        }
        SDL_ReleaseGPUTransferBuffer(device, download2);
    }

    puts("[composite-probe] COMPLETED all three submissions");
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return 0;
}
