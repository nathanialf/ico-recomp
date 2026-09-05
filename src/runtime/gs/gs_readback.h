/* gs/gs_readback.h: scanout image -> PPM file, shared by the live backend's
 * ICORECOMP_GS_SCREENSHOT path (gs_parallel_scanout.cpp) and the headless
 * replay entry point (gs_parallel_abi.cpp).
 *
 * Only compiled into targets that link libicorecomp-parallel-gs.so. The
 * image must already be in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL (both
 * callers request that layout in VSyncInfo). Synchronous: submits a copy
 * and fence-waits, so callers keep it off hot paths.
 *
 * PPM output of game scanout is ROM-derived data; paths must stay outside
 * the repository (check_no_rom is the mechanical gate for the repo side).
 *
 * The pixels are the scanout as the GS produced it, with no aspect ratio
 * correction: PS2 pixels are not square, so the image's width:height is not
 * its shape on screen. That is the point, it keeps the file a function of the
 * GS output alone. Presentation applies the display aspect separately; see
 * scanout_display_aspect in gs_parallel_scanout.cpp.
 */
#ifndef ICORECOMP_GS_READBACK_H
#define ICORECOMP_GS_READBACK_H

#include "device.hpp"
#include "image.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

/* Returns true on success. Expects an R8G8B8A8_UNORM 2D image in
 * current_layout (a barrier to TRANSFER_SRC is inserted when needed; the
 * image is left in TRANSFER_SRC_OPTIMAL).
 *
 * `why` receives the reason on a false return, so the caller's log line can
 * say which of the seven ways this can fail happened rather than "failed".
 * Every write is checked and a partial file is removed: this is the
 * regression baseline for rendering parity, so a truncated PPM that reported
 * success would be compared against as if it were a picture. */
inline bool rt_gs_write_scanout_ppm(Vulkan::Device& device, Vulkan::Image& image, const char* path,
                                    VkImageLayout current_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    std::string* why = nullptr) {
    auto fail = [why](std::string reason) {
        if (why) *why = std::move(reason);
        return false;
    };
    const uint32_t w = image.get_width();
    const uint32_t h = image.get_height();
    if (!w || !h) return fail("the scanout image is empty");

    Vulkan::BufferCreateInfo bi = {};
    bi.domain = Vulkan::BufferDomain::CachedHost;
    bi.size = VkDeviceSize(w) * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Vulkan::BufferHandle buf = device.create_buffer(bi, nullptr);
    if (!buf) return fail("the readback staging buffer could not be created");

    auto cmd = device.request_command_buffer();
    if (current_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        cmd->image_barrier(image, current_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cmd->copy_image_to_buffer(*buf, image, 0, { 0, 0, 0 }, { w, h, 1 }, 0, 0, layers);
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    Vulkan::Fence fence;
    device.submit(cmd, &fence);
    fence->wait();

    const uint8_t* px = static_cast<const uint8_t*>(
        device.map_host_buffer(*buf, Vulkan::MEMORY_ACCESS_READ_BIT));
    if (!px) return fail("the readback staging buffer would not map");

    errno = 0;
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        const int e = errno;
        device.unmap_host_buffer(*buf, Vulkan::MEMORY_ACCESS_READ_BIT);
        return fail(std::string("the file could not be opened for writing: ")
                    + std::strerror(e));
    }
    bool ok = std::fprintf(f, "P6\n%u %u\n255\n", w, h) > 0;
    for (uint32_t i = 0; ok && i < w * h; ++i) {
        ok = std::fwrite(px + i * 4, 1, 3, f) == 3; /* drop alpha */
    }
    const int write_errno = ok ? 0 : errno;
    /* fclose flushes, so a full disk can fail here and nowhere else. */
    if (std::fclose(f) != 0 && ok) {
        ok = false;
    }
    device.unmap_host_buffer(*buf, Vulkan::MEMORY_ACCESS_READ_BIT);
    if (!ok) {
        /* A short PPM is worse than no PPM: the parity comparison would read
         * it as a picture. */
        std::remove(path);
        return fail(std::string("the pixels could not be written and the partial file was "
                                "removed: ")
                    + std::strerror(write_errno ? write_errno : ENOSPC));
    }
    return true;
}

#endif /* ICORECOMP_GS_READBACK_H */
