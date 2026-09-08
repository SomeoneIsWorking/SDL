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
