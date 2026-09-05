/* rhi/metal/rhi_metal_cmd.mm: command list recording.
 *
 * Ours (MIT).
 *
 * Three decisions worth stating up front, because each one is a place where
 * Metal's model and rhi.h's do not line up on their own.
 *
 * Encoders. Metal has one encoder open on a command buffer at a time and no
 * way to interleave two kinds, where Vulkan lets a copy and a dispatch be
 * recorded next to each other. The list therefore keeps whichever encoder it
 * has open and closes it when an operation needs a different one. Closing an
 * encoder is also a dependency: Metal orders the work of two encoders on one
 * command buffer and makes the first one's writes visible to the second for
 * every resource with hazard tracking on, which is every resource this
 * backend creates.
 *
 * Barriers inside an encoder. The compute encoders are created with
 * MTLDispatchTypeConcurrent, so two dispatches in one encoder may overlap
 * unless something orders them, and rhi.h's buffer_barrier and
 * texture_barrier are what does: each becomes a memoryBarrierWithScope on the
 * open compute encoder. That is the faithful reading of rhi.h, whose contract
 * is that the caller states every dependency, and it is the same contract the
 * Vulkan backend enforces with vkCmdPipelineBarrier2. The alternative,
 * MTLDispatchTypeSerial, orders every dispatch against the one before it
 * whether the caller asked or not; it would paper over a missing barrier here
 * that the Vulkan backend would report as a race. If a dispatch pair ever
 * turns out to need an order rhi.h was never told about, the one-line change
 * is the dispatch type in compute_encoder() and the bug is in the caller.
 *
 * Bindings. They stand until changed and are written into the encoder at each
 * dispatch and draw, from the bindings standing at that moment. Nothing is
 * cached and nothing is reused, so a binding changed between two dispatches
 * cannot leak into the first one. Metal's argument tables are per encoder, so
 * a new encoder starts empty and this is what refills it.
 */
#import "rhi_metal.h"

#include "../../runtime.h"

namespace rhi {

namespace {

uint32_t bytes_per_pixel(const MetalDevice* dev, MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatRGBA8Unorm:
        case MTLPixelFormatBGRA8Unorm:
        case MTLPixelFormatR32Uint:
            return 4;
        default:
            dev->fatal("a copy touched a texture whose pixel size this backend does not "
                       "know (MTLPixelFormat %lu)", (unsigned long)f);
    }
}

} // namespace

void MetalCommandList::reset(id<MTLCommandBuffer> cb) {
    m_cb = cb;
    m_compute = nil;
    m_blit = nil;
    m_render = nil;
    m_encoder = Encoder::None;
    for (BufferBinding& b : m_ubo) b = BufferBinding{};
    for (BufferBinding& b : m_ssbo) b = BufferBinding{};
    for (Texture*& t : m_tex) t = nullptr;
    for (Texture*& t : m_image) t = nullptr;
    std::memset(m_push, 0, sizeof(m_push));
    m_compute_pipeline = nullptr;
    m_index_buffer = nullptr;
    m_index_offset = 0;
    m_render_width = 0;
    m_render_height = 0;
    m_touched_swapchain = false;
}

void MetalCommandList::end_encoder() {
    switch (m_encoder) {
        case Encoder::Compute: [m_compute endEncoding]; m_compute = nil; break;
        case Encoder::Blit:    [m_blit endEncoding];    m_blit = nil;    break;
        case Encoder::Render:  [m_render endEncoding];  m_render = nil;  break;
        case Encoder::None:    break;
    }
    m_encoder = Encoder::None;
}

id<MTLComputeCommandEncoder> MetalCommandList::compute_encoder() {
    if (m_encoder == Encoder::Compute) return m_compute;
    end_encoder();
    /* Concurrent, not serial. The comment at the top of this file says why. */
    m_compute = [m_cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
    if (!m_compute) m_dev->fatal("computeCommandEncoderWithDispatchType returned nothing");
    m_encoder = Encoder::Compute;
    return m_compute;
}

id<MTLBlitCommandEncoder> MetalCommandList::blit_encoder() {
    if (m_encoder == Encoder::Blit) return m_blit;
    end_encoder();
    m_blit = [m_cb blitCommandEncoder];
    if (!m_blit) m_dev->fatal("blitCommandEncoder returned nothing");
    m_encoder = Encoder::Blit;
    return m_blit;
}

/* ---- bindings ------------------------------------------------------------- */

void MetalCommandList::bind_uniform_buffer(uint32_t slot, Buffer* b,
                                           uint64_t offset, uint64_t range) {
    if (slot >= kMaxUniformBuffers) m_dev->fatal("uniform buffer slot %u is out of range", slot);
    /* 256 and not 4. A uniform buffer reaches the shader in Metal's constant
     * address space, where MTLGPUFamilyMac2 requires the argument's offset to
     * be a multiple of 256 while Apple silicon requires only 4. The strict
     * rule is enforced on both so that a binding that works on one accepted
     * family works on the other, rather than a Mac2 device failing on a
     * binding an M-series machine accepted. */
    if (b && (offset % 256) != 0) {
        m_dev->fatal("uniform buffer slot %u bound at offset %llu; Metal wants a multiple of "
                     "256 there (MTLGPUFamilyMac2's constant buffer alignment)", slot,
                     (unsigned long long)offset);
    }
    m_ubo[slot].buffer = b;
    m_ubo[slot].offset = offset;
    m_ubo[slot].range = range;
}

void MetalCommandList::bind_storage_buffer(uint32_t slot, Buffer* b,
                                           uint64_t offset, uint64_t range) {
    if (slot >= kMaxStorageBuffers) m_dev->fatal("storage buffer slot %u is out of range", slot);
    if (b && (offset % 4) != 0) {
        m_dev->fatal("storage buffer slot %u bound at offset %llu; Metal wants a multiple of "
                     "4 there", slot, (unsigned long long)offset);
    }
    /* range is recorded and not enforced. A Metal buffer argument carries an
     * offset and no length, so there is nothing to write it into: a shader
     * that reads past the range reads the rest of the buffer instead of being
     * caught, which the Vulkan backend's VkDescriptorBufferInfo.range and its
     * validation layer would catch. Nothing in the renderer depends on the
     * bound length; this is written down because the difference is invisible
     * until it matters. */
    m_ssbo[slot].buffer = b;
    m_ssbo[slot].offset = offset;
    m_ssbo[slot].range = range;
}

void MetalCommandList::bind_texture(uint32_t slot, Texture* t) {
    if (slot >= kMaxSampledTextures) m_dev->fatal("texture slot %u is out of range", slot);
    m_tex[slot] = t;
}

void MetalCommandList::bind_storage_image(uint32_t slot, Texture* t) {
    if (slot >= kMaxStorageImages) m_dev->fatal("storage image slot %u is out of range", slot);
    m_image[slot] = t;
}

void MetalCommandList::push_constants(const void* data, size_t bytes) {
    if (bytes > kPushConstantBytes) {
        m_dev->fatal("%zu bytes of push constants, and the budget is %u", bytes,
                     (unsigned)kPushConstantBytes);
    }
    /* Copied and not sent: Metal's setBytes writes into the encoder, and the
     * encoder for this draw or dispatch may not exist yet. The whole 128
     * bytes go out at the dispatch, so a partial push leaves the rest of the
     * block at whatever the previous push put there, exactly as
     * vkCmdPushConstants with a smaller range does. */
    std::memcpy(m_push, data, bytes);
}

void MetalCommandList::apply_compute_bindings(id<MTLComputeCommandEncoder> enc) {
    for (uint32_t i = 0; i < kMaxUniformBuffers; ++i) {
        Buffer* b = m_ubo[i].buffer ? m_ubo[i].buffer : m_dev->dummy_buffer();
        const NSUInteger off = m_ubo[i].buffer ? (NSUInteger)m_ubo[i].offset : 0;
        [enc setBuffer:b->buffer offset:off atIndex:metal_bind::kUniformBufferBaseIndex + i];
    }
    for (uint32_t i = 0; i < kMaxStorageBuffers; ++i) {
        Buffer* b = m_ssbo[i].buffer ? m_ssbo[i].buffer : m_dev->dummy_buffer();
        const NSUInteger off = m_ssbo[i].buffer ? (NSUInteger)m_ssbo[i].offset : 0;
        [enc setBuffer:b->buffer offset:off atIndex:metal_bind::kStorageBufferBaseIndex + i];
    }
    for (uint32_t i = 0; i < kMaxSampledTextures; ++i) {
        Texture* t = m_tex[i] ? m_tex[i] : m_dev->dummy_texture();
        [enc setTexture:t->texture atIndex:metal_bind::kSampledTextureBaseIndex + i];
    }
    for (uint32_t i = 0; i < kMaxStorageImages; ++i) {
        Texture* t = m_image[i] ? m_image[i] : m_dev->dummy_storage_image();
        [enc setTexture:t->texture atIndex:metal_bind::kStorageImageBaseIndex + i];
    }
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
        [enc setSamplerState:m_dev->sampler(i) atIndex:metal_bind::kSamplerBaseIndex + i];
    }
    [enc setBytes:m_push length:kPushConstantBytes atIndex:metal_bind::kPushConstantIndex];
}

void MetalCommandList::apply_render_bindings(id<MTLRenderCommandEncoder> enc) {
    /* Both stages get the whole set: rhi.h's one descriptor set layout gives
     * every binding VK_SHADER_STAGE_ALL, and Metal's argument tables are per
     * stage, so the same slot has to be written twice. */
    for (uint32_t i = 0; i < kMaxUniformBuffers; ++i) {
        Buffer* b = m_ubo[i].buffer ? m_ubo[i].buffer : m_dev->dummy_buffer();
        const NSUInteger off = m_ubo[i].buffer ? (NSUInteger)m_ubo[i].offset : 0;
        const NSUInteger index = metal_bind::kUniformBufferBaseIndex + i;
        [enc setVertexBuffer:b->buffer offset:off atIndex:index];
        [enc setFragmentBuffer:b->buffer offset:off atIndex:index];
    }
    for (uint32_t i = 0; i < kMaxStorageBuffers; ++i) {
        Buffer* b = m_ssbo[i].buffer ? m_ssbo[i].buffer : m_dev->dummy_buffer();
        const NSUInteger off = m_ssbo[i].buffer ? (NSUInteger)m_ssbo[i].offset : 0;
        const NSUInteger index = metal_bind::kStorageBufferBaseIndex + i;
        [enc setVertexBuffer:b->buffer offset:off atIndex:index];
        [enc setFragmentBuffer:b->buffer offset:off atIndex:index];
    }
    for (uint32_t i = 0; i < kMaxSampledTextures; ++i) {
        Texture* t = m_tex[i] ? m_tex[i] : m_dev->dummy_texture();
        const NSUInteger index = metal_bind::kSampledTextureBaseIndex + i;
        [enc setVertexTexture:t->texture atIndex:index];
        [enc setFragmentTexture:t->texture atIndex:index];
    }
    for (uint32_t i = 0; i < kMaxStorageImages; ++i) {
        Texture* t = m_image[i] ? m_image[i] : m_dev->dummy_storage_image();
        const NSUInteger index = metal_bind::kStorageImageBaseIndex + i;
        [enc setVertexTexture:t->texture atIndex:index];
        [enc setFragmentTexture:t->texture atIndex:index];
    }
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
        const NSUInteger index = metal_bind::kSamplerBaseIndex + i;
        [enc setVertexSamplerState:m_dev->sampler(i) atIndex:index];
        [enc setFragmentSamplerState:m_dev->sampler(i) atIndex:index];
    }
    [enc setVertexBytes:m_push length:kPushConstantBytes
                atIndex:metal_bind::kPushConstantIndex];
    [enc setFragmentBytes:m_push length:kPushConstantBytes
                  atIndex:metal_bind::kPushConstantIndex];
}

/* ---- compute -------------------------------------------------------------- */

void MetalCommandList::bind_compute_pipeline(ComputePipeline* p) {
    if (!p) m_dev->fatal("bind_compute_pipeline with no pipeline");
    /* Recorded rather than set: the encoder this dispatch will use may not
     * have been created yet, and Metal's pipeline state lives on the
     * encoder. */
    m_compute_pipeline = p;
}

void MetalCommandList::dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!m_compute_pipeline) m_dev->fatal("dispatch with no compute pipeline bound");
    if (gx == 0 || gy == 0 || gz == 0) return;
    id<MTLComputeCommandEncoder> enc = compute_encoder();
    [enc setComputePipelineState:m_compute_pipeline->pso];
    apply_compute_bindings(enc);
    [enc dispatchThreadgroups:MTLSizeMake(gx, gy, gz)
        threadsPerThreadgroup:m_compute_pipeline->threads_per_group];
}

void MetalCommandList::dispatch_indirect(Buffer* args, uint64_t offset) {
    if (!args) m_dev->fatal("dispatch_indirect with no argument buffer");
    if (!m_compute_pipeline) m_dev->fatal("dispatch_indirect with no compute pipeline bound");
    if ((offset % 4) != 0) {
        m_dev->fatal("dispatch_indirect at offset %llu; Metal wants a multiple of 4",
                     (unsigned long long)offset);
    }
    id<MTLComputeCommandEncoder> enc = compute_encoder();
    [enc setComputePipelineState:m_compute_pipeline->pso];
    apply_compute_bindings(enc);
    /* Three 32-bit group counts with a 12-byte stride, which is what a
     * VkDispatchIndirectCommand holds and what the D3D12 command signature
     * declares, so the caller's buffer is laid out identically for all
     * three. */
    [enc dispatchThreadgroupsWithIndirectBuffer:args->buffer
                           indirectBufferOffset:(NSUInteger)offset
                          threadsPerThreadgroup:m_compute_pipeline->threads_per_group];
}

/* ---- graphics ------------------------------------------------------------- */

void MetalCommandList::begin_render_pass(Texture* color, bool clear,
                                         float r, float g, float b, float a) {
    if (!color) m_dev->fatal("begin_render_pass with no colour attachment");
    if (m_encoder == Encoder::Render) m_dev->fatal("begin_render_pass inside a render pass");
    if (!color->owns_texture) m_touched_swapchain = true;
    end_encoder();

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = color->texture;
    rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, a);

    m_render = [m_cb renderCommandEncoderWithDescriptor:rp];
    if (!m_render) m_dev->fatal("renderCommandEncoderWithDescriptor returned nothing");
    m_encoder = Encoder::Render;
    m_render_width = color->width;
    m_render_height = color->height;

    /* The viewport and scissor have no default that means the whole
     * attachment, so the whole attachment is set here and a caller that wants
     * less overrides it. Same as the Vulkan backend's dynamic state. */
    set_viewport(0.0f, 0.0f, (float)color->width, (float)color->height);
    set_scissor(0, 0, color->width, color->height);
}

void MetalCommandList::end_render_pass() {
    if (m_encoder != Encoder::Render) return;
    end_encoder();
    m_render_width = 0;
    m_render_height = 0;
}

void MetalCommandList::bind_graphics_pipeline(GraphicsPipeline* p) {
    if (!p) m_dev->fatal("bind_graphics_pipeline with no pipeline");
    if (m_encoder != Encoder::Render) {
        m_dev->fatal("bind_graphics_pipeline outside a render pass; Metal keeps pipeline "
                     "state on the render encoder, so there is nowhere to put it");
    }
    [m_render setRenderPipelineState:p->pso];
}

void MetalCommandList::bind_vertex_buffer(Buffer* b, uint64_t offset) {
    if (!b) m_dev->fatal("bind_vertex_buffer with no buffer");
    if (m_encoder != Encoder::Render) m_dev->fatal("bind_vertex_buffer outside a render pass");
    [m_render setVertexBuffer:b->buffer
                       offset:(NSUInteger)offset
                      atIndex:metal_bind::kVertexBufferIndex];
}

void MetalCommandList::bind_index_buffer(Buffer* b, uint64_t offset) {
    if (!b) m_dev->fatal("bind_index_buffer with no buffer");
    /* Recorded: Metal takes the index buffer at the draw, not as state. */
    m_index_buffer = b;
    m_index_offset = offset;
}

void MetalCommandList::set_viewport(float x, float y, float w, float h) {
    if (m_encoder != Encoder::Render) m_dev->fatal("set_viewport outside a render pass");
    MTLViewport vp;
    vp.originX = x;
    vp.originY = y;
    vp.width = w;
    vp.height = h;
    vp.znear = 0.0;
    vp.zfar = 1.0;
    [m_render setViewport:vp];
}

void MetalCommandList::set_scissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (m_encoder != Encoder::Render) m_dev->fatal("set_scissor outside a render pass");

    /* Metal rejects a scissor rectangle that leaves the attachment, where
     * Vulkan clamps one silently. Clamping here is a divergence from what the
     * caller asked for, so it is logged with the rectangle and the attachment
     * rather than done quietly. Nothing in the renderer or the UI is known to
     * hit it; a line in the log means one of them now does. */
    int64_t x0 = x;
    int64_t y0 = y;
    int64_t x1 = (int64_t)x + (int64_t)w;
    int64_t y1 = (int64_t)y + (int64_t)h;
    const int64_t rw = (int64_t)m_render_width;
    const int64_t rh = (int64_t)m_render_height;
    if (x0 < 0 || y0 < 0 || x1 > rw || y1 > rh) {
        rt_log_warn("rhi", "scissor (%d,%d %ux%u) leaves the %ux%u attachment and Metal will "
                           "not take it; clamped", x, y, w, h, m_render_width, m_render_height);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > rw) x1 = rw;
        if (y1 > rh) y1 = rh;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
    }

    MTLScissorRect rect;
    rect.x = (NSUInteger)x0;
    rect.y = (NSUInteger)y0;
    rect.width = (NSUInteger)(x1 - x0);
    rect.height = (NSUInteger)(y1 - y0);
    [m_render setScissorRect:rect];
}

void MetalCommandList::draw_indexed(uint32_t index_count, uint32_t first_index,
                                    int32_t vertex_offset) {
    if (m_encoder != Encoder::Render) m_dev->fatal("draw_indexed outside a render pass");
    if (!m_index_buffer) m_dev->fatal("draw_indexed with no index buffer bound");
    if (index_count == 0) return;
    apply_render_bindings(m_render);
    /* 32-bit indices: the UI emits uint32 index data (RtPgsOverlayFrame), the
     * same as the Vulkan backend's VK_INDEX_TYPE_UINT32. */
    [m_render drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexCount:index_count
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:m_index_buffer->buffer
                  indexBufferOffset:(NSUInteger)(m_index_offset + (uint64_t)first_index * 4)
                      instanceCount:1
                         baseVertex:vertex_offset
                       baseInstance:0];
}

/* ---- copies ---------------------------------------------------------------
 *
 * Metal's buffer to texture copies take a row pitch of the caller's choosing
 * and place the footprint at any 4-byte offset, so rhi.h's tightly packed
 * contract needs no padding and no scratch buffer. That is the one place this
 * backend is simpler than the D3D12 one, where a 256-byte row pitch and a
 * 512-byte placement rule force a second buffer and a row-by-row repack.
 */

void MetalCommandList::copy_buffer(Buffer* dst, uint64_t dst_offset,
                                   Buffer* src, uint64_t src_offset, uint64_t bytes) {
    if (!dst || !src) m_dev->fatal("copy_buffer with a null buffer");
    if (bytes == 0) return;
    id<MTLBlitCommandEncoder> enc = blit_encoder();
    [enc copyFromBuffer:src->buffer
           sourceOffset:(NSUInteger)src_offset
               toBuffer:dst->buffer
      destinationOffset:(NSUInteger)dst_offset
                   size:(NSUInteger)bytes];
}

void MetalCommandList::copy_buffer_to_texture(Texture* dst, Buffer* src, uint64_t src_offset) {
    if (!dst || !src) m_dev->fatal("copy_buffer_to_texture with a null resource");
    const uint32_t bpp = bytes_per_pixel(m_dev, dst->format);
    if ((src_offset % bpp) != 0) {
        m_dev->fatal("copy_buffer_to_texture from offset %llu into a %u byte per pixel "
                     "texture; Metal wants the offset to be a multiple of the pixel size",
                     (unsigned long long)src_offset, bpp);
    }
    const NSUInteger row = (NSUInteger)dst->width * bpp;
    id<MTLBlitCommandEncoder> enc = blit_encoder();
    [enc copyFromBuffer:src->buffer
           sourceOffset:(NSUInteger)src_offset
      sourceBytesPerRow:row
    sourceBytesPerImage:row * dst->height
             sourceSize:MTLSizeMake(dst->width, dst->height, 1)
              toTexture:dst->texture
       destinationSlice:0
       destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
}

void MetalCommandList::copy_texture_to_buffer(Buffer* dst, uint64_t dst_offset, Texture* src) {
    if (!dst || !src) m_dev->fatal("copy_texture_to_buffer with a null resource");
    if (!src->owns_texture) m_touched_swapchain = true;
    const uint32_t bpp = bytes_per_pixel(m_dev, src->format);
    if ((dst_offset % bpp) != 0) {
        m_dev->fatal("copy_texture_to_buffer to offset %llu from a %u byte per pixel "
                     "texture; Metal wants the offset to be a multiple of the pixel size",
                     (unsigned long long)dst_offset, bpp);
    }
    const NSUInteger row = (NSUInteger)src->width * bpp;
    id<MTLBlitCommandEncoder> enc = blit_encoder();
    [enc copyFromTexture:src->texture
             sourceSlice:0
             sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(src->width, src->height, 1)
                toBuffer:dst->buffer
       destinationOffset:(NSUInteger)dst_offset
  destinationBytesPerRow:row
destinationBytesPerImage:row * src->height];
}

void MetalCommandList::blit_texture(Texture* dst, int32_t dx, int32_t dy,
                                    uint32_t dw, uint32_t dh, Texture* src,
                                    bool linear_filter) {
    if (!dst || !src) m_dev->fatal("blit_texture with a null texture");
    if (!dst->owns_texture || !src->owns_texture) m_touched_swapchain = true;
    if (dw == 0 || dh == 0) return;
    end_encoder();

    /* A draw and not a copy: MTLBlitCommandEncoder's texture copies neither
     * scale nor filter, so the present blit is one full-screen triangle with
     * the destination rectangle as the viewport and the scissor. The D3D12
     * backend does the same thing for the same reason; Vulkan is the odd one
     * out, with vkCmdBlitImage. */
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst->texture;
    /* Load and not clear: a blit into part of the destination must leave the
     * rest of it alone. */
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> enc = [m_cb renderCommandEncoderWithDescriptor:rp];
    if (!enc) m_dev->fatal("the present blit could not open a render encoder");

    MTLViewport vp;
    vp.originX = (double)dx;
    vp.originY = (double)dy;
    vp.width = (double)dw;
    vp.height = (double)dh;
    vp.znear = 0.0;
    vp.zfar = 1.0;
    [enc setViewport:vp];

    /* Metal rejects a scissor rectangle that leaves the attachment, so the
     * destination rectangle is clamped to it here the same way set_scissor
     * clamps, and logged the same way: a present rectangle wider than the
     * drawable would otherwise be a hard error from Metal rather than a
     * logged clamp. The viewport above is left as the caller gave it, so the
     * picture keeps the scale it asked for and only the drawn area is cut. */
    int64_t sx0 = dx;
    int64_t sy0 = dy;
    int64_t sx1 = (int64_t)dx + (int64_t)dw;
    int64_t sy1 = (int64_t)dy + (int64_t)dh;
    const int64_t aw = (int64_t)dst->width;
    const int64_t ah = (int64_t)dst->height;
    if (sx0 < 0 || sy0 < 0 || sx1 > aw || sy1 > ah) {
        rt_log_warn("rhi", "blit destination (%d,%d %ux%u) leaves the %ux%u attachment and "
                           "Metal will not take it; clamped",
                    dx, dy, dw, dh, dst->width, dst->height);
        if (sx0 < 0) sx0 = 0;
        if (sy0 < 0) sy0 = 0;
        if (sx1 > aw) sx1 = aw;
        if (sy1 > ah) sy1 = ah;
        if (sx1 < sx0) sx1 = sx0;
        if (sy1 < sy0) sy1 = sy0;
    }

    MTLScissorRect rect;
    rect.x = (NSUInteger)sx0;
    rect.y = (NSUInteger)sy0;
    rect.width = (NSUInteger)(sx1 - sx0);
    rect.height = (NSUInteger)(sy1 - sy0);
    [enc setScissorRect:rect];

    [enc setRenderPipelineState:m_dev->blit_pipeline(dst->format)->pso];
    /* Every element of the shader's texture and sampler arrays is written,
     * not just slot 0: an argument Metal never saw is undefined, and the
     * dummy costs one pointer write. */
    for (uint32_t i = 0; i < kMaxSampledTextures; ++i) {
        id<MTLTexture> t = i == 0 ? src->texture : m_dev->dummy_texture()->texture;
        [enc setFragmentTexture:t atIndex:metal_bind::kSampledTextureBaseIndex + i];
    }
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
        [enc setFragmentSamplerState:m_dev->sampler(i)
                             atIndex:metal_bind::kSamplerBaseIndex + i];
    }

    /* The filter as a push constant, matching the D3D12 blit's root constant:
     * a dynamic index into a sampler array is an argument buffer feature and
     * this shader has no need of one. */
    uint32_t constants[kPushConstantBytes / sizeof(uint32_t)] = {};
    constants[0] = linear_filter ? 1u : 0u;
    [enc setFragmentBytes:constants length:sizeof(constants)
                  atIndex:metal_bind::kPushConstantIndex];

    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    m_encoder = Encoder::None;
}

/* ---- barriers -------------------------------------------------------------
 *
 * Two cases, and the comment at the top of this file says why they are the
 * two.
 *
 * Inside a compute encoder, which is where every dependency the renderer
 * states actually lands, the barrier becomes a memoryBarrierWithScope. The
 * encoder dispatches concurrently, so this is the whole of the ordering
 * between two dispatches.
 *
 * Anywhere else there is nothing to do. Metal orders the work of two encoders
 * on one command buffer and makes the first's writes visible to the second
 * for every resource with hazard tracking on, which is every resource this
 * backend creates, so the encoder switch the next operation forces is already
 * the dependency. The rhi.h Stage and Access arguments carry no further
 * information here: Metal's scope is the resource kind and not the pipeline
 * stage.
 */

void MetalCommandList::buffer_barrier(Buffer* b, Stage from, Access fa,
                                      Stage to, Access ta) {
    (void)b;
    (void)from;
    (void)fa;
    (void)to;
    (void)ta;
    if (m_encoder != Encoder::Compute) return;
    [m_compute memoryBarrierWithScope:MTLBarrierScopeBuffers];
}

void MetalCommandList::texture_barrier(Texture* t, Stage from, Access fa,
                                       Stage to, Access ta) {
    (void)from;
    (void)fa;
    (void)to;
    (void)ta;
    if (t && !t->owns_texture) m_touched_swapchain = true;
    if (m_encoder != Encoder::Compute) return;
    [m_compute memoryBarrierWithScope:MTLBarrierScopeTextures];
}

} // namespace rhi
