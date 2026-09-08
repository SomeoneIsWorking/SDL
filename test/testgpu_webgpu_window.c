/* Browser window/swapchain probe; no application data or audio. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <emscripten.h>
#include <stdio.h>

static SDL_Window *window;
static SDL_GPUDevice *device;

static void pump_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
            SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
            SDL_DestroyWindow(window);
            SDL_Quit();
            emscripten_cancel_main_loop();
            puts("WebGPU window: closed");
            return;
        }
    }
}

int main(int argc, char **argv)
{
    SDL_GPUTexture *texture;
    SDL_GPUCommandBuffer *command;
    SDL_GPURenderPass *render;
    SDL_GPUColorTargetInfo target = { 0 };
    (void)argc;
    (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 1;
    }
    window = SDL_CreateWindow("WebGPU canvas", 64, 64, 0);
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_WGSL, true, "webgpu");
    if (!window || !device || !SDL_ClaimWindowForGPUDevice(device, window)) {
        printf("WebGPU window creation FAILED: %s\n", SDL_GetError());
        return 1;
    }
    command = SDL_AcquireGPUCommandBuffer(device);
    if (!command || !SDL_WaitAndAcquireGPUSwapchainTexture(command, window, &texture, NULL, NULL) || !texture) {
        printf("WebGPU swapchain FAILED: %s\n", SDL_GetError());
        return 1;
    }
    target.texture = texture;
    target.clear_color.b = 1;
    target.clear_color.a = 1;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    render = SDL_BeginGPURenderPass(command, &target, 1, NULL);
    SDL_EndGPURenderPass(render);
    if (!SDL_SubmitGPUCommandBuffer(command) || !SDL_WaitForGPUIdle(device)) {
        printf("WebGPU window submission FAILED: %s\n", SDL_GetError());
        return 1;
    }
    puts("WebGPU window: presented blue canvas");
    emscripten_set_main_loop(pump_events, 0, 1);
    return 0;
}
