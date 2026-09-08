/* Offscreen WebGPU submission/readback contract, with no game data. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <emscripten.h>
#include <stdio.h>

EM_JS(void, instrument_queue_submission, (), {
    const submit = GPUQueue.prototype.submit;
    const fence = GPUQueue.prototype.onSubmittedWorkDone;
    globalThis.queueSubmissionEvents = [];
    GPUQueue.prototype.submit = function(... args)
    {
        queueSubmissionEvents.push("submit");
        return submit.apply(this, args);
    };
    GPUQueue.prototype.onSubmittedWorkDone = function(... args)
    {
        queueSubmissionEvents.push("fence");
        return fence.apply(this, args);
    };
});
EM_JS(void, begin_submission_check, (), { queueSubmissionEvents = []; });
EM_JS(int, submitted_before_fences, (), {
    return queueSubmissionEvents.indexOf("submit") == 0 && queueSubmissionEvents.includes("fence");
});

static int check_download(SDL_GPUTextureType type, Uint32 layer, Uint32 mip, Uint32 pitch)
{
    SDL_GPUDevice *device = NULL;
    SDL_GPUTexture *texture = NULL;
    SDL_GPUTransferBuffer *download = NULL;
    SDL_GPUFence *fence = NULL;
    SDL_GPUCommandBuffer *command;
    SDL_GPURenderPass *render;
    SDL_GPUCopyPass *copy;
    SDL_GPUTextureCreateInfo texture_info = { 0 };
    SDL_GPUTransferBufferCreateInfo transfer_info = { 0 };
    SDL_GPUColorTargetInfo target = { 0 };
    SDL_GPUTextureRegion region = { 0 };
    SDL_GPUTextureTransferInfo destination = { 0 };
    const Uint8 *pixels;
    Uint32 matched = 0;
    int result = 1;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        goto cleanup;
    }
    puts("WebGPU probe: creating device");
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_WGSL, true, "webgpu");
    if (!device) {
        goto cleanup;
    }
    printf("WebGPU probe: backend=%s\n", SDL_GetGPUDeviceDriver(device));
    texture_info.type = type;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    texture_info.width = 4 << mip;
    texture_info.height = 4 << mip;
    texture_info.layer_count_or_depth = layer + 1;
    texture_info.num_levels = mip + 1;
    texture = SDL_CreateGPUTexture(device, &texture_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = 1024;
    download = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (!texture || !download) {
        goto cleanup;
    }
    command = SDL_AcquireGPUCommandBuffer(device);
    if (!command) {
        goto cleanup;
    }
    target.texture = texture;
    target.layer_or_depth_plane = layer;
    target.mip_level = mip;
    target.clear_color.r = 1.0f;
    target.clear_color.a = 1.0f;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    render = SDL_BeginGPURenderPass(command, &target, 1, NULL);
    if (!render) {
        SDL_CancelGPUCommandBuffer(command);
        goto cleanup;
    }
    SDL_EndGPURenderPass(render);
    copy = SDL_BeginGPUCopyPass(command);
    region.texture = texture;
    region.layer = layer;
    region.mip_level = mip;
    region.w = 4;
    region.h = 4;
    region.d = 1;
    destination.transfer_buffer = download;
    destination.pixels_per_row = pitch;
    destination.rows_per_layer = 4;
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);
    begin_submission_check();
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (!submitted_before_fences()) {
        puts("WebGPU queue ordering FAILED: completion fence registered before current submission");
        goto cleanup;
    }
    if (!fence || !SDL_WaitForGPUFences(device, true, &fence, 1)) {
        goto cleanup;
    }
    pixels = SDL_MapGPUTransferBuffer(device, download, false);
    if (!pixels) {
        goto cleanup;
    }
    for (Uint32 y = 0; y < 4; ++y) {
        for (Uint32 x = 0; x < 4; ++x) {
            const Uint8 *pixel = pixels + y * (pitch ? pitch : 4) * 4 + x * 4;
            matched += pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255;
        }
    }
    SDL_UnmapGPUTransferBuffer(device, download);
    printf("WebGPU probe: layer=%u mip=%u pitch=%u matched %u/16 red pixels\n", layer, mip, pitch, matched);
    result = matched == 16 ? 0 : 1;
cleanup:
    if (result) {
        printf("WebGPU probe FAILED: %s\n", SDL_GetError());
    }
    if (fence) {
        SDL_ReleaseGPUFence(device, fence);
    }
    if (download) {
        SDL_ReleaseGPUTransferBuffer(device, download);
    }
    if (texture) {
        SDL_ReleaseGPUTexture(device, texture);
    }
    if (device) {
        SDL_DestroyGPUDevice(device);
    }
    SDL_Quit();
    printf("WebGPU probe: completed result=%d\n", result);
    return result;
}

int main(int argc, char **argv)
{
    int failures = 0;
    (void)argc;
    (void)argv;
    instrument_queue_submission();
    failures += check_download(SDL_GPU_TEXTURETYPE_2D, 0, 0, 64);
    failures += check_download(SDL_GPU_TEXTURETYPE_2D, 0, 0, 0);
    failures += check_download(SDL_GPU_TEXTURETYPE_2D_ARRAY, 1, 1, 64);
    failures += check_download(SDL_GPU_TEXTURETYPE_2D_ARRAY, 1, 1, 7);
    printf("WebGPU download: 4 cases, %d failures\n", failures);
    return failures ? 1 : 0;
}
