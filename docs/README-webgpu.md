# Experimental WebGPU backend

This fork retains the SDL_GPU interface. It accepts WGSL shaders and consumes
Emscripten's pinned `emdawnwebgpu` port when cross-compiling for browsers.
It is not yet a qualified replacement for every native GPU backend.

## Worker build and focused verification

With Emscripten 4.0.16 activated, configure a separate build directory:

```sh
emcmake cmake -S . -B build/webgpu -G Ninja -DSDL_STATIC=ON -DSDL_SHARED=OFF \
  -DSDL_WEBGPU=ON -DSDL_PTHREADS=ON -DSDL_TESTS=ON \
  -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread
cmake --build build/webgpu --target testgpu_fences testgpu_webgpu
emrun --no_browser --port 8767 build/webgpu/test/testgpu_webgpu.html
```

The GPU test runs on an Emscripten pthread with Asyncify, because the backend
waits synchronously for JavaScript adapter, device and mapping callbacks. The
server must supply cross-origin isolation headers; `emrun` does. Use a browser
with an available WebGPU adapter. Device creation alone is insufficient: the
test requires four render-clear/download cases to each return 16/16 expected
pixels, covering packed and padded rows and nonzero array layer/mip selection.

`testgpu_fences` tests the production fence-set waiting policy against seven
completion timelines, including a fast fence preceding a slow one. The old
policy counted the fast fence again on each poll and could return from wait-all
before all fences completed. The texture download path also formerly used the
source layer as the copy depth, making ordinary layer-zero downloads copy zero
slices; the browser test failed with 0/16 expected pixels before that repair.

These checks passed in headless Chromium through WebLua on Linux with
Emscripten 4.0.16. They establish synthetic submission/readback and fence policy,
not complete renderer, shader, presentation, thread-safety, or game conformance.

The `testgpu_webgpu_window` browser probe creates and claims an SDL window,
submits a blue swapchain clear, then retains the presentation until Escape or
window close releases the resources. Its worker requires
`-sOFFSCREENCANVAS_SUPPORT -sOFFSCREENCANVASES_TO_PTHREAD=#canvas` in addition to
Asyncify/pthread flags. Without canvas transfer the browser worker cannot find
the canvas for WebGPU context creation.

A real blue canvas was observed through WebLua with Chromium under an isolated
Xvfb display. The same Linux headless Chromium run submitted successfully but
its screenshot omitted both this canvas and an independent raw WebGPU canvas;
headless screenshot color alone is therefore not a trusted GPU discriminator.
Texture readback remains independently checked by `testgpu_webgpu`.
