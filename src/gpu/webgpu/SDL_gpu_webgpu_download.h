/* WebGPU texture download layout: SDL pitches need not be 256-byte aligned. */
#ifndef SDL_gpu_webgpu_download_h_
#define SDL_gpu_webgpu_download_h_

#include "../SDL_sysgpu.h"
#include "webgpu.h"

static inline void WEBGPU_DownloadTextureRegion(WGPUCommandEncoder encoder, WGPUTexture texture,
                                                WGPUBuffer buffer, SDL_GPUTextureType type,
                                                SDL_GPUTextureFormat format,
                                                const SDL_GPUTextureRegion *source,
                                                const SDL_GPUTextureTransferInfo *destination)
{
    const Uint32 block_height = Texture_GetBlockHeight(format);
    const Uint32 pitch = BytesPerRow(destination->pixels_per_row ? destination->pixels_per_row : source->w, format);
    const Uint32 image_rows = destination->rows_per_layer ? destination->rows_per_layer : source->h;
    const Uint32 block_rows = (image_rows + block_height - 1) / block_height;
    WGPUTexelCopyTextureInfo source_info = { 0 };
    WGPUTexelCopyBufferInfo destination_info = { 0 };
    WGPUExtent3D extent = { source->w, source->h, source->d };
    source_info.texture = texture;
    source_info.mipLevel = source->mip_level;
    source_info.aspect = WGPUTextureAspect_All;
    source_info.origin = (WGPUOrigin3D){ source->x, source->y, type == SDL_GPU_TEXTURETYPE_3D ? source->z : source->layer };
    destination_info.buffer = buffer;
    destination_info.layout.offset = destination->offset;
    if (pitch % 256 == 0) {
        destination_info.layout.bytesPerRow = pitch;
        destination_info.layout.rowsPerImage = block_rows;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &source_info, &destination_info, &extent);
        return;
    }
    // WebGPU permits an unaligned pitch when copying one block row and one
    // slice at a time with bytesPerRow/rowsPerImage omitted. Preserve the
    // caller's packed layout instead of silently writing a padded image.
    destination_info.layout.bytesPerRow = WGPU_COPY_STRIDE_UNDEFINED;
    destination_info.layout.rowsPerImage = WGPU_COPY_STRIDE_UNDEFINED;
    extent.depthOrArrayLayers = 1;
    for (Uint32 z = 0; z < source->d; ++z) {
        for (Uint32 y = 0; y < source->h; y += block_height) {
            source_info.origin.y = source->y + y;
            extent.height = SDL_min(block_height, source->h - y);
            destination_info.layout.offset = destination->offset + (Uint64)z * block_rows * pitch + (Uint64)(y / block_height) * pitch;
            wgpuCommandEncoderCopyTextureToBuffer(encoder, &source_info, &destination_info, &extent);
        }
        ++source_info.origin.z;
    }
}
#endif
