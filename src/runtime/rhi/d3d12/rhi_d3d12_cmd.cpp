/* rhi/d3d12/rhi_d3d12_cmd.cpp: command list recording.
 *
 * Ours (MIT).
 *
 * The same two decisions the Vulkan backend states, for the same reason: both
 * trade a little GPU work for a class of bug this project cannot debug by
 * running the renderer locally.
 *
 *   A descriptor table is written per dispatch and per draw, from the
 *   bindings standing at that moment, into the frame's slice of the one
 *   shader-visible heap. Nothing is cached, so a binding changed between two
 *   dispatches can never leak into the first.
 *
 *   Resource state transitions live inside the operations that need them,
 *   driven by the state each resource is tracked as being in. buffer_barrier
 *   and texture_barrier stay public for the compute to graphics handoff,
 *   which is the one dependency the caller alone knows about.
 *
 * A note on the two root argument sets: D3D12 keeps the graphics and compute
 * root arguments apart, so push_constants stores the bytes and the dispatch
 * or draw writes them onto whichever pipeline is about to run. The Vulkan
 * backend can push once because a Vulkan pipeline layout is shared by both
 * bind points.
 */
#include "rhi_d3d12.h"

#include "../../runtime.h"

namespace rhi {

namespace {

/* The state a texture has to be in for a given use. There is no equivalent of
 * Vulkan's GENERAL: a D3D12 resource is either a UAV or a shader resource,
 * never both at once, so a storage image sits in UNORDERED_ACCESS and a
 * sampled one in the two shader-resource states. */
D3D12_RESOURCE_STATES texture_state_for(Stage s, Access a) {
    const bool read_only = a == Access::Read;
    switch (s) {
        case Stage::Compute:
            return read_only ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                             : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case Stage::Graphics:
            return read_only ? (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                             : D3D12_RESOURCE_STATE_RENDER_TARGET;
        case Stage::Copy:
            return read_only ? D3D12_RESOURCE_STATE_COPY_SOURCE
                             : D3D12_RESOURCE_STATE_COPY_DEST;
        case Stage::Present:
            return D3D12_RESOURCE_STATE_PRESENT;
        case Stage::Host:
            return D3D12_RESOURCE_STATE_COMMON;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

/* The same for a buffer. Every buffer the shaders touch is a UAV
 * (rhi_d3d12_bindings.h says why the storage buffers are UAVs and not SRVs),
 * so a shader stage means UNORDERED_ACCESS whichever access was asked for. */
D3D12_RESOURCE_STATES buffer_state_for(Stage s, Access a) {
    const bool read_only = a == Access::Read;
    switch (s) {
        case Stage::Compute:
        case Stage::Graphics:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case Stage::Copy:
            return read_only ? D3D12_RESOURCE_STATE_COPY_SOURCE
                             : D3D12_RESOURCE_STATE_COPY_DEST;
        case Stage::Host:
        case Stage::Present:
            return D3D12_RESOURCE_STATE_COMMON;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

} // namespace

void D3D12CommandList::reset(ID3D12GraphicsCommandList* list) {
    m_list = list;
    for (BufferBinding& b : m_ubo) b = BufferBinding{};
    for (BufferBinding& b : m_ssbo) b = BufferBinding{};
    for (Texture*& t : m_tex) t = nullptr;
    for (Texture*& t : m_image) t = nullptr;
    std::memset(m_push, 0, sizeof(m_push));
    m_in_render_pass = false;
    m_touched_swapchain = false;
}

/* ---- state transitions ---------------------------------------------------- */

void D3D12CommandList::transition(Texture* t, D3D12_RESOURCE_STATES to) {
    if (!t) return;
    if (t->swapchain) m_touched_swapchain = true;
    if (t->state == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t->resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t->state;
    b.Transition.StateAfter = to;
    m_list->ResourceBarrier(1, &b);
    t->state = to;
}

void D3D12CommandList::transition(Buffer* b, D3D12_RESOURCE_STATES to) {
    if (!b) return;
    if (!b->tracked) {
        /* An upload or readback heap resource has a state D3D12 fixes for its
         * whole life. GENERIC_READ already covers every read a copy or the
         * input assembler can ask for, and COPY_DEST covers the one write a
         * readback buffer receives. Anything else is a caller error and is
         * named rather than turned into an illegal barrier. */
        const bool ok = (b->state == D3D12_RESOURCE_STATE_GENERIC_READ
                         && to != D3D12_RESOURCE_STATE_COPY_DEST
                         && to != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                     || (b->state == D3D12_RESOURCE_STATE_COPY_DEST
                         && to == D3D12_RESOURCE_STATE_COPY_DEST);
        if (!ok) {
            m_dev->fatal("a %s buffer cannot be moved to resource state 0x%x; D3D12 fixes "
                         "the state of a host-visible heap resource",
                         b->kind == BufferKind::Upload ? "upload" : "readback", (unsigned)to);
        }
        return;
    }
    if (b->state == to) return;
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = b->resource;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = b->state;
    bar.Transition.StateAfter = to;
    m_list->ResourceBarrier(1, &bar);
    b->state = to;
}

void D3D12CommandList::uav_barrier(ID3D12Resource* res) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    m_list->ResourceBarrier(1, &b);
}

/* ---- bindings ------------------------------------------------------------- */

void D3D12CommandList::bind_uniform_buffer(uint32_t slot, Buffer* b,
                                           uint64_t offset, uint64_t range) {
    if (slot >= kMaxUniformBuffers) m_dev->fatal("uniform buffer slot %u is out of range", slot);
    if (b && (offset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) != 0) {
        /* Not rounded down: a constant buffer view that starts somewhere the
         * caller did not ask for would read the wrong bytes silently. */
        m_dev->fatal("uniform buffer slot %u was bound at offset %llu, and D3D12 requires a "
                     "constant buffer view to start on a %u byte boundary", slot,
                     (unsigned long long)offset,
                     (unsigned)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    }
    m_ubo[slot].buffer = b;
    m_ubo[slot].offset = offset;
    m_ubo[slot].range = range;
}

void D3D12CommandList::bind_storage_buffer(uint32_t slot, Buffer* b,
                                           uint64_t offset, uint64_t range) {
    if (slot >= kMaxStorageBuffers) m_dev->fatal("storage buffer slot %u is out of range", slot);
    if (b && (offset % 4) != 0) {
        m_dev->fatal("storage buffer slot %u was bound at offset %llu; a raw view addresses "
                     "32-bit words, so the offset has to be a multiple of 4", slot,
                     (unsigned long long)offset);
    }
    m_ssbo[slot].buffer = b;
    m_ssbo[slot].offset = offset;
    m_ssbo[slot].range = range;
    /* A host-visible storage buffer lives twice; the bound range is copied
     * into the default-heap shadow here, so the shader reads what the CPU had
     * written at the moment of the bind. See Buffer::staging. */
    if (b && b->shadowed) {
        m_dev->flush_storage_shadow(m_list, b, offset, range ? range : b->size - offset);
    }
}

void D3D12CommandList::bind_texture(uint32_t slot, Texture* t) {
    if (slot >= kMaxSampledTextures) m_dev->fatal("texture slot %u is out of range", slot);
    m_tex[slot] = t;
}

void D3D12CommandList::bind_storage_image(uint32_t slot, Texture* t) {
    if (slot >= kMaxStorageImages) m_dev->fatal("storage image slot %u is out of range", slot);
    m_image[slot] = t;
}

void D3D12CommandList::push_constants(const void* data, size_t bytes) {
    if (bytes > kPushConstantBytes) {
        m_dev->fatal("%zu bytes of push constants, and the budget is %u", bytes,
                     (unsigned)kPushConstantBytes);
    }
    /* Stored, not written onto the root signature here: D3D12 keeps the
     * graphics and compute root arguments apart, so the dispatch or draw
     * writes these bytes onto whichever pipeline is about to run. */
    std::memcpy(m_push, data, bytes);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12CommandList::build_descriptor_table() {
    ID3D12Device* dev = m_dev->d3d();
    const uint32_t stride = m_dev->descriptor_size();

    /* Every resource has to be in the state its descriptor class needs before
     * the dispatch or draw reads it. Doing it here rather than in the bind
     * keeps a rebind that changes nothing from emitting a barrier. */
    for (uint32_t i = 0; i < kMaxUniformBuffers; ++i) {
        if (m_ubo[i].buffer) {
            transition(m_ubo[i].buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
    }
    for (uint32_t i = 0; i < kMaxStorageBuffers; ++i) {
        if (m_ssbo[i].buffer) transition(m_ssbo[i].buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    for (uint32_t i = 0; i < kMaxSampledTextures; ++i) {
        if (m_tex[i]) {
            transition(m_tex[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                               | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    for (uint32_t i = 0; i < kMaxStorageImages; ++i) {
        if (m_image[i]) transition(m_image[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    m_dev->allocate_descriptor_set(&cpu, &gpu);
    auto slot_handle = [&](uint32_t index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
        h.ptr += (SIZE_T)index * stride;
        return h;
    };

    /* Every descriptor of the table is written, including the slots nothing
     * was bound to: a table with a hole is undefined on resource binding
     * tiers 1 and 2, so the unbound slots take the device's dummies, exactly
     * as the Vulkan backend fills its arrays. */
    for (uint32_t i = 0; i < kMaxUniformBuffers; ++i) {
        Buffer* b = m_ubo[i].buffer ? m_ubo[i].buffer : m_dev->dummy_uniform_buffer();
        const uint64_t offset = m_ubo[i].buffer ? m_ubo[i].offset : 0;
        uint64_t range = m_ubo[i].buffer ? m_ubo[i].range : b->size;
        if (range == 0) range = b->size - offset;
        D3D12_CONSTANT_BUFFER_VIEW_DESC cb{};
        cb.BufferLocation = b->resource->GetGPUVirtualAddress() + offset;
        /* A constant buffer view's size is rounded up to 256; the extra bytes
         * are never read by a shader that respects its own block size. */
        cb.SizeInBytes = (UINT)align_up(range, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        dev->CreateConstantBufferView(&cb, slot_handle(d3d12_bind::kTableUniformBuffers + i));
    }

    for (uint32_t i = 0; i < kMaxStorageBuffers; ++i) {
        Buffer* b = m_ssbo[i].buffer ? m_ssbo[i].buffer : m_dev->dummy_buffer();
        const uint64_t offset = m_ssbo[i].buffer ? m_ssbo[i].offset : 0;
        uint64_t range = m_ssbo[i].buffer ? m_ssbo[i].range : b->size;
        if (range == 0) range = b->size - offset;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        /* A raw view: R32_TYPELESS with the RAW flag, which is what an HLSL
         * RWByteAddressBuffer is, and what SPIRV-Cross emits for a GLSL
         * std430 storage block of uints. */
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = offset / 4;
        uav.Buffer.NumElements = (UINT)(range / 4);
        uav.Buffer.StructureByteStride = 0;
        uav.Buffer.CounterOffsetInBytes = 0;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        dev->CreateUnorderedAccessView(b->resource, nullptr, &uav,
                                       slot_handle(d3d12_bind::kTableStorageBuffers + i));
    }

    for (uint32_t i = 0; i < kMaxSampledTextures; ++i) {
        Texture* t = m_tex[i] ? m_tex[i] : m_dev->dummy_texture();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = t->format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(t->resource, &srv,
                                      slot_handle(d3d12_bind::kTableSampledTextures + i));
    }

    for (uint32_t i = 0; i < kMaxStorageImages; ++i) {
        Texture* t = m_image[i] ? m_image[i] : m_dev->dummy_storage_image();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = t->format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(t->resource, nullptr, &uav,
                                       slot_handle(d3d12_bind::kTableStorageImages + i));
    }
    return gpu;
}

/* ---- compute -------------------------------------------------------------- */

void D3D12CommandList::bind_compute_pipeline(ComputePipeline* p) {
    if (!p) m_dev->fatal("bind_compute_pipeline with no pipeline");
    m_list->SetPipelineState(p->pso);
}

void D3D12CommandList::dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    const D3D12_GPU_DESCRIPTOR_HANDLE table = build_descriptor_table();
    m_list->SetComputeRoot32BitConstants(d3d12_bind::kRootParamConstants,
                                         d3d12_bind::kRootConstantDwords, m_push, 0);
    m_list->SetComputeRootDescriptorTable(d3d12_bind::kRootParamTable, table);
    m_list->Dispatch(gx, gy, gz);
}

void D3D12CommandList::dispatch_indirect(Buffer* args, uint64_t offset) {
    if (!args) m_dev->fatal("dispatch_indirect with no argument buffer");
    transition(args, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    const D3D12_GPU_DESCRIPTOR_HANDLE table = build_descriptor_table();
    m_list->SetComputeRoot32BitConstants(d3d12_bind::kRootParamConstants,
                                         d3d12_bind::kRootConstantDwords, m_push, 0);
    m_list->SetComputeRootDescriptorTable(d3d12_bind::kRootParamTable, table);
    m_list->ExecuteIndirect(m_dev->dispatch_signature(), 1, args->resource, offset,
                            nullptr, 0);
}

/* ---- graphics ------------------------------------------------------------- */

void D3D12CommandList::begin_render_pass(Texture* color, bool clear,
                                         float r, float g, float b, float a) {
    if (!color) m_dev->fatal("begin_render_pass with no colour attachment");
    if (m_in_render_pass) m_dev->fatal("begin_render_pass inside a render pass");
    if (color->rtv == UINT32_MAX) {
        m_dev->fatal("begin_render_pass on a texture that was not created as a colour "
                     "target, so it has no render target view");
    }
    transition(color, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_dev->rtv_handle(color->rtv);
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    if (clear) {
        const float rgba[4] = { r, g, b, a };
        m_list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
    }
    m_in_render_pass = true;

    /* Viewport and scissor have no default; the whole attachment is set here
     * and a caller that wants less overrides it, as on Vulkan. */
    set_viewport(0.0f, 0.0f, (float)color->width, (float)color->height);
    set_scissor(0, 0, color->width, color->height);
}

void D3D12CommandList::end_render_pass() {
    /* Nothing to close: OMSetRenderTargets is state, not a scope. The flag is
     * kept so begin_render_pass can catch a nesting mistake the same way the
     * Vulkan backend does. */
    m_in_render_pass = false;
}

void D3D12CommandList::bind_graphics_pipeline(GraphicsPipeline* p) {
    if (!p) m_dev->fatal("bind_graphics_pipeline with no pipeline");
    m_list->SetPipelineState(p->pso);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void D3D12CommandList::bind_vertex_buffer(Buffer* b, uint64_t offset) {
    if (!b) m_dev->fatal("bind_vertex_buffer with no buffer");
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = b->resource->GetGPUVirtualAddress() + offset;
    v.SizeInBytes = (UINT)(b->size - offset);
    /* The one vertex layout, documented in rhi.h: float2 position, float2
     * texture coordinate, one packed RGBA8 colour. 20 bytes. */
    v.StrideInBytes = 20;
    m_list->IASetVertexBuffers(0, 1, &v);
}

void D3D12CommandList::bind_index_buffer(Buffer* b, uint64_t offset) {
    if (!b) m_dev->fatal("bind_index_buffer with no buffer");
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = b->resource->GetGPUVirtualAddress() + offset;
    v.SizeInBytes = (UINT)(b->size - offset);
    /* 32-bit indices: the UI emits uint32 index data (RtPgsOverlayFrame). */
    v.Format = DXGI_FORMAT_R32_UINT;
    m_list->IASetIndexBuffer(&v);
}

void D3D12CommandList::set_viewport(float x, float y, float w, float h) {
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_list->RSSetViewports(1, &vp);
}

void D3D12CommandList::set_scissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    D3D12_RECT r{};
    r.left = x;
    r.top = y;
    r.right = x + (LONG)w;
    r.bottom = y + (LONG)h;
    m_list->RSSetScissorRects(1, &r);
}

void D3D12CommandList::draw_indexed(uint32_t index_count, uint32_t first_index,
                                    int32_t vertex_offset) {
    const D3D12_GPU_DESCRIPTOR_HANDLE table = build_descriptor_table();
    m_list->SetGraphicsRoot32BitConstants(d3d12_bind::kRootParamConstants,
                                          d3d12_bind::kRootConstantDwords, m_push, 0);
    m_list->SetGraphicsRootDescriptorTable(d3d12_bind::kRootParamTable, table);
    m_list->DrawIndexedInstanced(index_count, 1, first_index, vertex_offset, 0);
}

/* ---- copies ---------------------------------------------------------------
 *
 * rhi.h's contract is that a buffer holds a texture tightly packed. D3D12
 * lays a texture into a buffer with each row padded to
 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256) and the whole footprint placed on
 * a D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512) boundary. Where those
 * happen to match what the caller asked for the copy is direct; otherwise it
 * goes through the device's scratch buffer and is packed or unpacked one row
 * at a time. That is the whole cost of the difference, and it lands only on
 * the overlay texture upload and the screenshot path, neither of which is per
 * field.
 */

void D3D12CommandList::copy_buffer(Buffer* dst, uint64_t dst_offset,
                                   Buffer* src, uint64_t src_offset, uint64_t bytes) {
    if (!dst || !src) m_dev->fatal("copy_buffer with a null buffer");
    if (bytes == 0) return;
    if (src->shadowed) m_dev->flush_storage_shadow(m_list, src, src_offset, bytes);
    transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    m_list->CopyBufferRegion(dst->resource, dst_offset, src->resource, src_offset, bytes);
}

void D3D12CommandList::copy_buffer_to_texture(Texture* dst, Buffer* src, uint64_t src_offset) {
    if (!dst || !src) m_dev->fatal("copy_buffer_to_texture with a null resource");

    const D3D12_RESOURCE_DESC rd = dst->resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    m_dev->d3d()->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_bytes, &total);

    /* The same flush copy_buffer does, for the same reason: a host-visible
     * buffer that also carries Storage usage gets a default-heap shadow on
     * this backend, and src->resource is that shadow. Without the flush this
     * copy reads whatever the last bind_storage_buffer put there rather than
     * the bytes the CPU just wrote, and says nothing about it. No caller does
     * it today (overlay_texture_create asks for Upload | CopySrc), which is
     * why it was a divergence between two neighbouring functions rather than
     * a live bug. The span is the packed rows this call reads, rows *
     * row_bytes, not the padded footprint. */
    if (src->shadowed) {
        m_dev->flush_storage_shadow(m_list, src, src_offset, (uint64_t)rows * row_bytes);
    }

    const bool packed = fp.Footprint.RowPitch == row_bytes;
    const bool placed = (src_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) == 0;

    ID3D12Resource* source = src->resource;
    uint64_t source_offset = src_offset;
    if (!packed || !placed) {
        ID3D12Resource* scratch = m_dev->scratch(total);
        if (m_dev->scratch_state() != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = scratch;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = m_dev->scratch_state();
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            m_list->ResourceBarrier(1, &b);
            m_dev->set_scratch_state(D3D12_RESOURCE_STATE_COPY_DEST);
        }
        transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT y = 0; y < rows; ++y) {
            m_list->CopyBufferRegion(scratch, (uint64_t)y * fp.Footprint.RowPitch,
                                     src->resource, src_offset + (uint64_t)y * row_bytes,
                                     row_bytes);
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = scratch;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_list->ResourceBarrier(1, &b);
        m_dev->set_scratch_state(D3D12_RESOURCE_STATE_COPY_SOURCE);
        source = scratch;
        source_offset = 0;
    } else {
        transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = source;
    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s.PlacedFootprint = fp;
    s.PlacedFootprint.Offset = source_offset;
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = dst->resource;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.SubresourceIndex = 0;
    m_list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
}

void D3D12CommandList::copy_texture_to_buffer(Buffer* dst, uint64_t dst_offset, Texture* src) {
    if (!dst || !src) m_dev->fatal("copy_texture_to_buffer with a null resource");

    const D3D12_RESOURCE_DESC rd = src->resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    m_dev->d3d()->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_bytes, &total);

    const bool packed = fp.Footprint.RowPitch == row_bytes;
    const bool placed = (dst_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) == 0;

    transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src->resource;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;

    if (packed && placed) {
        transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = dst->resource;
        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        d.PlacedFootprint = fp;
        d.PlacedFootprint.Offset = dst_offset;
        m_list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        return;
    }

    ID3D12Resource* scratch = m_dev->scratch(total);
    if (m_dev->scratch_state() != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = scratch;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = m_dev->scratch_state();
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        m_list->ResourceBarrier(1, &b);
        m_dev->set_scratch_state(D3D12_RESOURCE_STATE_COPY_DEST);
    }
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = scratch;
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = fp;
    d.PlacedFootprint.Offset = 0;
    m_list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = scratch;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_list->ResourceBarrier(1, &b);
    m_dev->set_scratch_state(D3D12_RESOURCE_STATE_COPY_SOURCE);

    transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT y = 0; y < rows; ++y) {
        m_list->CopyBufferRegion(dst->resource, dst_offset + (uint64_t)y * row_bytes,
                                 scratch, (uint64_t)y * fp.Footprint.RowPitch, row_bytes);
    }
}

/* ---- blit -----------------------------------------------------------------
 *
 * D3D12 has no equivalent of vkCmdBlitImage: CopyTextureRegion neither scales
 * nor filters. The present blit is therefore a draw of one full-screen
 * triangle placed by the viewport, with the source in sampled texture slot 0
 * and the filter chosen by a root constant. The shader is the backend's own
 * (rhi_d3d12_shaders.cpp), because there is no GLSL for it on the Vulkan
 * side to cross-compile.
 */
void D3D12CommandList::blit_texture(Texture* dst, int32_t dx, int32_t dy,
                                    uint32_t dw, uint32_t dh, Texture* src,
                                    bool linear_filter) {
    if (!dst || !src) m_dev->fatal("blit_texture with a null texture");
    if (m_in_render_pass) m_dev->fatal("blit_texture inside a render pass");
    if (dst->rtv == UINT32_MAX) {
        m_dev->fatal("blit_texture into a texture that was not created as a colour target; "
                     "the D3D12 present blit is a draw and needs a render target view");
    }
    if (!has(src->usage, TextureUsage::Sampled)) {
        m_dev->fatal("blit_texture from a texture that was not created as sampled; the "
                     "D3D12 present blit reads it through a shader resource view");
    }
    if (dst->swapchain || src->swapchain) m_touched_swapchain = true;

    transition(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                  | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(dst, D3D12_RESOURCE_STATE_RENDER_TARGET);

    /* The bindings the blit needs, saved and put back, so a blit between two
     * draws does not disturb what the caller had standing. */
    Texture* saved_tex0 = m_tex[0];
    uint8_t saved_push[kPushConstantBytes];
    std::memcpy(saved_push, m_push, sizeof(saved_push));

    m_tex[0] = src;
    std::memset(m_push, 0, sizeof(m_push));
    const uint32_t filter = linear_filter ? 1u : 0u;
    std::memcpy(m_push, &filter, sizeof(filter));

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_dev->rtv_handle(dst->rtv);
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_list->SetPipelineState(m_dev->blit_pipeline(dst->format)->pso);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    set_viewport((float)dx, (float)dy, (float)dw, (float)dh);
    set_scissor(dx, dy, dw, dh);

    const D3D12_GPU_DESCRIPTOR_HANDLE table = build_descriptor_table();
    m_list->SetGraphicsRoot32BitConstants(d3d12_bind::kRootParamConstants,
                                          d3d12_bind::kRootConstantDwords, m_push, 0);
    m_list->SetGraphicsRootDescriptorTable(d3d12_bind::kRootParamTable, table);
    m_list->DrawInstanced(3, 1, 0, 0);

    m_tex[0] = saved_tex0;
    std::memcpy(m_push, saved_push, sizeof(saved_push));

    /* The blit's pipeline state and its render target are still bound to the
     * command list. Vulkan's vkCmdBlitImage leaves no such state, so a caller
     * that draws next without bind_graphics_pipeline would get the blit
     * pipeline here and its own on the other backend. Nothing in the renderer
     * does that: every blit is the last thing in its command list. Restoring
     * would mean tracking a pipeline this class does not otherwise hold, so
     * the contract is written down instead: after blit_texture, bind a
     * pipeline and a render target before drawing. */
}

/* ---- barriers ------------------------------------------------------------- */

void D3D12CommandList::buffer_barrier(Buffer* b, Stage from, Access fa,
                                      Stage to, Access ta) {
    if (!b) {
        /* The caller asked for a dependency and gets none. Not fatal, because
         * the buffer being null usually means the pass that would have used
         * it did nothing either, but silent it must not be: a missing barrier
         * is a race that shows up as one wrong field somewhere else. Once,
         * since a caller that does this does it every field. */
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_warn("rhi", "buffer_barrier with no buffer (stage %u to %u); no "
                               "dependency is recorded for it",
                        (unsigned)from, (unsigned)to);
        }
        return;
    }
    const D3D12_RESOURCE_STATES want = buffer_state_for(to, ta);
    const D3D12_RESOURCE_STATES had = buffer_state_for(from, fa);
    if (want == had && want == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        /* Two shader accesses of the same resource: D3D12 spells that
         * dependency as a UAV barrier, not a transition. */
        uav_barrier(b->resource);
        return;
    }
    transition(b, want);
}

void D3D12CommandList::texture_barrier(Texture* t, Stage from, Access fa,
                                       Stage to, Access ta) {
    if (!t) {
        /* Same as buffer_barrier above: the dependency the caller asked for
         * is not recorded, and nothing else would ever say so. */
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_warn("rhi", "texture_barrier with no texture (stage %u to %u); no "
                               "dependency is recorded for it",
                        (unsigned)from, (unsigned)to);
        }
        return;
    }
    if (t->swapchain) m_touched_swapchain = true;
    const D3D12_RESOURCE_STATES want = texture_state_for(to, ta);
    const D3D12_RESOURCE_STATES had = texture_state_for(from, fa);
    if (want == had && want == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        uav_barrier(t->resource);
        return;
    }
    transition(t, want);
}

} // namespace rhi
