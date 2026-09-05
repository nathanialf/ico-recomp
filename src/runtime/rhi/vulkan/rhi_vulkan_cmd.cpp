/* rhi/vulkan/rhi_vulkan_cmd.cpp: command list recording.
 *
 * Ours (MIT).
 *
 * Two decisions worth stating, because both trade a little GPU work for a
 * class of bug this project cannot debug by running the renderer locally:
 *
 *   Descriptor sets are built per dispatch and per draw, from the bindings
 *   standing at that moment. Nothing is cached and nothing is reused, so a
 *   binding changed between two dispatches can never leak into the first one.
 *
 *   Layout transitions live inside the operations that need them (the copies,
 *   the blit and the render pass), driven by the layout each texture is
 *   tracked as being in. texture_barrier stays public for the compute to
 *   graphics handoff on a storage image, which is the one dependency the
 *   caller alone knows about.
 */
#include "rhi_vulkan.h"

#include "../../runtime.h"

namespace rhi {

namespace {

VkPipelineStageFlags2 stage_bits(Stage s) {
    switch (s) {
        case Stage::Compute:  return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case Stage::Graphics: return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        case Stage::Copy:     return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        case Stage::Host:     return VK_PIPELINE_STAGE_2_HOST_BIT;
        case Stage::Present:  return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    /* Only reachable from a Stage outside the enum. ALL_COMMANDS is correct
     * but over-broad, so it is a barrier the caller did not ask for and the
     * substitution is named rather than made quietly. */
    rt_log_warn("rhi", "stage_bits: rhi::Stage %u is not one this backend knows; using "
                       "VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT", (unsigned)s);
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

VkAccessFlags2 access_bits(Stage s, Access a) {
    const bool read = a == Access::Read || a == Access::ReadWrite;
    const bool write = a == Access::Write || a == Access::ReadWrite;
    VkAccessFlags2 bits = 0;
    switch (s) {
        case Stage::Compute:
            if (read) bits |= VK_ACCESS_2_SHADER_READ_BIT;
            if (write) bits |= VK_ACCESS_2_SHADER_WRITE_BIT;
            break;
        case Stage::Graphics:
            if (read) bits |= VK_ACCESS_2_SHADER_READ_BIT;
            /* A write in the graphics stage of this RHI is a colour
             * attachment write: the only graphics pipeline kind it has writes
             * through the output merger, and no fragment shader in it stores.
             * SHADER_WRITE alone left the overlay's COLOR_ATTACHMENT_WRITE
             * outside the source scope of the one barrier record_present
             * builds from this table, which is the present transition, so the
             * layout change was allowed to run ahead of the drawing it was
             * meant to follow. Both bits are set rather than one swapped for
             * the other, because a later graphics pipeline that does store
             * would silently lose the first. */
            if (write) {
                bits |= VK_ACCESS_2_SHADER_WRITE_BIT
                      | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            }
            break;
        case Stage::Copy:
            if (read) bits |= VK_ACCESS_2_TRANSFER_READ_BIT;
            if (write) bits |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
            break;
        case Stage::Host:
            if (read) bits |= VK_ACCESS_2_HOST_READ_BIT;
            if (write) bits |= VK_ACCESS_2_HOST_WRITE_BIT;
            break;
        case Stage::Present:
            break;
        default:
            /* A Stage outside the enum. The barrier is emitted with no access
             * bits at all, which is a synchronisation hole rather than a slow
             * path, so this one is an error. */
            rt_log_error("rhi", "access_bits: rhi::Stage %u is not one this backend knows; the "
                                "barrier is emitted with no access mask", (unsigned)s);
            break;
    }
    return bits;
}

/* The layout a texture has to be in for a given use. Storage images stay in
 * GENERAL, which is what a compute shader writing local memory wants and what
 * avoids a transition per dispatch. */
VkImageLayout layout_for(Stage s, Access a) {
    switch (s) {
        case Stage::Compute:  return VK_IMAGE_LAYOUT_GENERAL;
        case Stage::Graphics:
            return a == Access::Read ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case Stage::Copy:
            return a == Access::Read ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                     : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case Stage::Present:  return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case Stage::Host:     return VK_IMAGE_LAYOUT_GENERAL;
    }
    /* A Stage outside the enum. GENERAL is legal for every use and wrong for
     * most of them, so the texture ends up in a layout nothing asked for. */
    rt_log_error("rhi", "layout_for: rhi::Stage %u is not one this backend knows; the texture "
                        "is moved to VK_IMAGE_LAYOUT_GENERAL", (unsigned)s);
    return VK_IMAGE_LAYOUT_GENERAL;
}

} // namespace

void VulkanCommandList::reset(VkCommandBuffer cb) {
    m_cb = cb;
    for (BufferBinding& b : m_ubo) b = BufferBinding{};
    for (BufferBinding& b : m_ssbo) b = BufferBinding{};
    for (Texture*& t : m_tex) t = nullptr;
    for (Texture*& t : m_image) t = nullptr;
    std::memset(m_push, 0, sizeof(m_push));
    m_in_render_pass = false;
    m_touched_swapchain = false;
}

/* ---- bindings ------------------------------------------------------------- */

void VulkanCommandList::bind_uniform_buffer(uint32_t slot, Buffer* b,
                                            uint64_t offset, uint64_t range) {
    if (slot >= kMaxUniformBuffers) m_dev->fatal("uniform buffer slot %u is out of range", slot);
    m_ubo[slot].buffer = b ? b->buffer : VK_NULL_HANDLE;
    m_ubo[slot].offset = offset;
    m_ubo[slot].range = range ? range : VK_WHOLE_SIZE;
}

void VulkanCommandList::bind_storage_buffer(uint32_t slot, Buffer* b,
                                            uint64_t offset, uint64_t range) {
    if (slot >= kMaxStorageBuffers) m_dev->fatal("storage buffer slot %u is out of range", slot);
    m_ssbo[slot].buffer = b ? b->buffer : VK_NULL_HANDLE;
    m_ssbo[slot].offset = offset;
    m_ssbo[slot].range = range ? range : VK_WHOLE_SIZE;
}

void VulkanCommandList::bind_texture(uint32_t slot, Texture* t) {
    if (slot >= kMaxSampledTextures) m_dev->fatal("texture slot %u is out of range", slot);
    m_tex[slot] = t;
}

void VulkanCommandList::bind_storage_image(uint32_t slot, Texture* t) {
    if (slot >= kMaxStorageImages) m_dev->fatal("storage image slot %u is out of range", slot);
    m_image[slot] = t;
}

void VulkanCommandList::push_constants(const void* data, size_t bytes) {
    if (bytes > kPushConstantBytes) {
        m_dev->fatal("%zu bytes of push constants, and the budget is %u", bytes,
                     (unsigned)kPushConstantBytes);
    }
    std::memcpy(m_push, data, bytes);
    vkCmdPushConstants(m_cb, m_dev->pipeline_layout(), VK_SHADER_STAGE_ALL, 0,
                       kPushConstantBytes, m_push);
}

VkDescriptorSet VulkanCommandList::build_descriptor_set() {
    /* Every bound texture has to be in the layout its descriptor declares
     * before the dispatch reads it: SHADER_READ_ONLY_OPTIMAL for a sampled
     * image, GENERAL for a storage image. Doing it here rather than in the
     * bind keeps a rebind that changes nothing from emitting a barrier, and
     * it is the same place the D3D12 backend moves resource states.
     *
     * This is what the scanout image needed and did not have. It is written
     * as a storage image (GENERAL), then handed to vkCmdBlitImage, which
     * leaves it in TRANSFER_SRC_OPTIMAL, and the next field bound it as a
     * storage image again with a descriptor that says GENERAL. The first
     * field was right and every field after it was reading an image in a
     * layout its own descriptor disagreed with.
     *
     * Not inside a render pass: an image memory barrier recorded inside a
     * dynamic rendering instance may not change a layout. Nothing needs one
     * there. The only draws are the overlay's, whose textures are put in
     * SHADER_READ_ONLY_OPTIMAL by the upload that created them and never
     * leave it, and the present blit, which is not a draw on this backend. */
    if (!m_in_render_pass) {
        for (uint32_t i = 0; i < kMaxSampledTextures; ++i) {
            if (m_tex[i]) {
                transition(m_tex[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT);
            }
        }
        for (uint32_t i = 0; i < kMaxStorageImages; ++i) {
            if (m_image[i]) {
                transition(m_image[i], VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            }
        }
    }

    VkDescriptorSet set = m_dev->allocate_descriptor_set();

    VkDescriptorBufferInfo ubo[kMaxUniformBuffers]{};
    VkDescriptorBufferInfo ssbo[kMaxStorageBuffers]{};
    VkDescriptorImageInfo tex[kMaxSampledTextures]{};
    VkDescriptorImageInfo images[kMaxStorageImages]{};

    /* Every array element has to be a valid descriptor: this backend does not
     * use partially bound descriptor indexing, so the unbound slots take the
     * device's dummy resources. */
    for (uint32_t i = 0; i < kMaxUniformBuffers; ++i) {
        ubo[i].buffer = m_ubo[i].buffer ? m_ubo[i].buffer : m_dev->dummy_buffer()->buffer;
        ubo[i].offset = m_ubo[i].buffer ? m_ubo[i].offset : 0;
        ubo[i].range = m_ubo[i].buffer ? m_ubo[i].range : VK_WHOLE_SIZE;
    }
    for (uint32_t i = 0; i < kMaxStorageBuffers; ++i) {
        ssbo[i].buffer = m_ssbo[i].buffer ? m_ssbo[i].buffer : m_dev->dummy_buffer()->buffer;
        ssbo[i].offset = m_ssbo[i].buffer ? m_ssbo[i].offset : 0;
        ssbo[i].range = m_ssbo[i].buffer ? m_ssbo[i].range : VK_WHOLE_SIZE;
    }
    for (uint32_t i = 0; i < kMaxSampledTextures; ++i) {
        Texture* t = m_tex[i] ? m_tex[i] : m_dev->dummy_texture();
        tex[i].imageView = t->view;
        tex[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    for (uint32_t i = 0; i < kMaxStorageImages; ++i) {
        Texture* t = m_image[i] ? m_image[i] : m_dev->dummy_storage_image();
        images[i].imageView = t->view;
        images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkWriteDescriptorSet writes[4]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = kMaxUniformBuffers;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = ubo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = kMaxStorageBuffers;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = ssbo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = kMaxSampledTextures;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[2].pImageInfo = tex;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 4;
    writes[3].descriptorCount = kMaxStorageImages;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[3].pImageInfo = images;
    /* Binding 3 is not written: its samplers are immutable in the layout. */

    vkUpdateDescriptorSets(m_dev->vk(), 4, writes, 0, nullptr);
    return set;
}

/* ---- compute -------------------------------------------------------------- */

void VulkanCommandList::bind_compute_pipeline(ComputePipeline* p) {
    if (!p) m_dev->fatal("bind_compute_pipeline with no pipeline");
    vkCmdBindPipeline(m_cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
}

void VulkanCommandList::dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    VkDescriptorSet set = build_descriptor_set();
    vkCmdBindDescriptorSets(m_cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_dev->pipeline_layout(),
                            0, 1, &set, 0, nullptr);
    vkCmdDispatch(m_cb, gx, gy, gz);
}

void VulkanCommandList::dispatch_indirect(Buffer* args, uint64_t offset) {
    if (!args) m_dev->fatal("dispatch_indirect with no argument buffer");
    VkDescriptorSet set = build_descriptor_set();
    vkCmdBindDescriptorSets(m_cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_dev->pipeline_layout(),
                            0, 1, &set, 0, nullptr);
    vkCmdDispatchIndirect(m_cb, args->buffer, offset);
}

/* ---- graphics ------------------------------------------------------------- */

void VulkanCommandList::begin_render_pass(Texture* color, bool clear,
                                          float r, float g, float b, float a) {
    if (!color) m_dev->fatal("begin_render_pass with no colour attachment");
    if (m_in_render_pass) m_dev->fatal("begin_render_pass inside a render pass");
    if (!color->owns_image) m_touched_swapchain = true;

    transition(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = color->view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color.float32[0] = r;
    att.clearValue.color.float32[1] = g;
    att.clearValue.color.float32[2] = b;
    att.clearValue.color.float32[3] = a;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset = { 0, 0 };
    ri.renderArea.extent = { color->width, color->height };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(m_cb, &ri);
    m_in_render_pass = true;

    /* Viewport and scissor are dynamic and have no default, so the whole
     * attachment is set here and a caller that wants less overrides it. */
    set_viewport(0.0f, 0.0f, (float)color->width, (float)color->height);
    set_scissor(0, 0, color->width, color->height);
}

void VulkanCommandList::end_render_pass() {
    if (!m_in_render_pass) {
        /* Nothing to end. Harmless on its own, but it means the caller's idea
         * of where the render pass boundaries are does not match this one's,
         * and the next begin_render_pass is the one that would fatal. */
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_warn("rhi", "end_render_pass outside a render pass; nothing was ended. "
                               "Said once.");
        }
        return;
    }
    vkCmdEndRendering(m_cb);
    m_in_render_pass = false;
}

void VulkanCommandList::bind_graphics_pipeline(GraphicsPipeline* p) {
    if (!p) m_dev->fatal("bind_graphics_pipeline with no pipeline");
    vkCmdBindPipeline(m_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
}

void VulkanCommandList::bind_vertex_buffer(Buffer* b, uint64_t offset) {
    if (!b) m_dev->fatal("bind_vertex_buffer with no buffer");
    const VkDeviceSize off = offset;
    vkCmdBindVertexBuffers(m_cb, 0, 1, &b->buffer, &off);
}

void VulkanCommandList::bind_index_buffer(Buffer* b, uint64_t offset) {
    if (!b) m_dev->fatal("bind_index_buffer with no buffer");
    /* 32-bit indices: the UI emits uint32 index data (RtPgsOverlayFrame). */
    vkCmdBindIndexBuffer(m_cb, b->buffer, offset, VK_INDEX_TYPE_UINT32);
}

void VulkanCommandList::set_viewport(float x, float y, float w, float h) {
    VkViewport vp{};
    vp.x = x;
    vp.y = y;
    vp.width = w;
    vp.height = h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(m_cb, 0, 1, &vp);
}

void VulkanCommandList::set_scissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    VkRect2D r{};
    r.offset = { x, y };
    r.extent = { w, h };
    vkCmdSetScissor(m_cb, 0, 1, &r);
}

void VulkanCommandList::draw_indexed(uint32_t index_count, uint32_t first_index,
                                     int32_t vertex_offset) {
    VkDescriptorSet set = build_descriptor_set();
    vkCmdBindDescriptorSets(m_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_dev->pipeline_layout(),
                            0, 1, &set, 0, nullptr);
    vkCmdDrawIndexed(m_cb, index_count, 1, first_index, vertex_offset, 0);
}

/* ---- copies --------------------------------------------------------------- */

void VulkanCommandList::copy_buffer(Buffer* dst, uint64_t dst_offset,
                                    Buffer* src, uint64_t src_offset, uint64_t bytes) {
    if (!dst || !src) m_dev->fatal("copy_buffer with a null buffer");
    if (bytes == 0) {
        /* vkCmdCopyBuffer rejects a zero-size region, so the copy is skipped.
         * A caller asking for it is computing a size it did not mean to. */
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_warn("rhi", "copy_buffer of 0 bytes; nothing is copied. Said once.");
        }
        return;
    }
    VkBufferCopy region{};
    region.srcOffset = src_offset;
    region.dstOffset = dst_offset;
    region.size = bytes;
    vkCmdCopyBuffer(m_cb, src->buffer, dst->buffer, 1, &region);
}

void VulkanCommandList::copy_buffer_to_texture(Texture* dst, Buffer* src, uint64_t src_offset) {
    if (!dst || !src) m_dev->fatal("copy_buffer_to_texture with a null resource");
    transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = src_offset;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { dst->width, dst->height, 1 };
    vkCmdCopyBufferToImage(m_cb, src->buffer, dst->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void VulkanCommandList::copy_texture_to_buffer(Buffer* dst, uint64_t dst_offset, Texture* src) {
    if (!dst || !src) m_dev->fatal("copy_texture_to_buffer with a null resource");
    if (!src->owns_image) m_touched_swapchain = true;
    transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = dst_offset;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { src->width, src->height, 1 };
    vkCmdCopyImageToBuffer(m_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst->buffer, 1, &region);
}

void VulkanCommandList::blit_texture(Texture* dst, int32_t dx, int32_t dy,
                                     uint32_t dw, uint32_t dh, Texture* src,
                                     bool linear_filter) {
    if (!dst || !src) m_dev->fatal("blit_texture with a null texture");
    if (!dst->owns_image || !src->owns_image) m_touched_swapchain = true;
    transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[0] = { 0, 0, 0 };
    region.srcOffsets[1] = { (int32_t)src->width, (int32_t)src->height, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[0] = { dx, dy, 0 };
    region.dstOffsets[1] = { dx + (int32_t)dw, dy + (int32_t)dh, 1 };
    vkCmdBlitImage(m_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

/* ---- barriers ------------------------------------------------------------- */

void VulkanCommandList::transition(Texture* t, VkImageLayout to,
                                   VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    if (t->layout == to) return;
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = stage;
    b.dstAccessMask = access;
    b.oldLayout = t->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(m_cb, &di);
    t->layout = to;
}

void VulkanCommandList::buffer_barrier(Buffer* b, Stage from, Access fa,
                                       Stage to, Access ta) {
    if (!b) {
        /* No barrier is recorded, so whatever dependency the caller was
         * expressing is not enforced. Quiet here means a race later. */
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_error("rhi", "buffer_barrier with a null buffer; no barrier is recorded and "
                                "the dependency it stood for is not enforced. Said once.");
        }
        return;
    }
    VkBufferMemoryBarrier2 bb{};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    bb.srcStageMask = stage_bits(from);
    bb.srcAccessMask = access_bits(from, fa);
    bb.dstStageMask = stage_bits(to);
    bb.dstAccessMask = access_bits(to, ta);
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = b->buffer;
    bb.offset = 0;
    bb.size = VK_WHOLE_SIZE;

    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.bufferMemoryBarrierCount = 1;
    di.pBufferMemoryBarriers = &bb;
    vkCmdPipelineBarrier2(m_cb, &di);
}

void VulkanCommandList::texture_barrier(Texture* t, Stage from, Access fa,
                                        Stage to, Access ta) {
    if (!t) {
        /* As above, and worse: this one also carries a layout transition, so
         * the texture stays in whatever layout it was in. */
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_error("rhi", "texture_barrier with a null texture; no barrier and no layout "
                                "transition are recorded. Said once.");
        }
        return;
    }
    if (!t->owns_image) m_touched_swapchain = true;
    const VkImageLayout target = layout_for(to, ta);
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = stage_bits(from);
    b.srcAccessMask = access_bits(from, fa);
    if (to == Stage::Present) {
        /* The transition into PRESENT_SRC_KHR is the one barrier whose source
         * scope has to cover every write still pending on the image, and the
         * caller cannot name them: record_present writes the backbuffer with
         * a blit (TRANSFER_WRITE in the BLIT stage) and then, when the menu is
         * up, with the overlay (COLOR_ATTACHMENT_WRITE in the output merger),
         * and one `from` value cannot be both. A layout transition is ordered
         * only against the barrier's own scopes, so a narrower source scope
         * lets the transition run before the writes and presents an image
         * whose contents the specification leaves undefined. ALL_COMMANDS and
         * MEMORY_WRITE is the same over-broad pair the internal transition()
         * helper above uses, for the same reason; it costs one full flush per
         * present and it is the last barrier of the frame. */
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    b.dstStageMask = stage_bits(to);
    b.dstAccessMask = access_bits(to, ta);
    b.oldLayout = t->layout;
    b.newLayout = target;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(m_cb, &di);
    t->layout = target;
}

} // namespace rhi
