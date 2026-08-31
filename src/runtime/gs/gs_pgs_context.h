/* gs/gs_pgs_context.h: headless Granite Vulkan context creation for
 * paraLLEl-GS consumers (gs_parallel.cpp, gs_replay_main.cpp).
 *
 * Flag policy: paraLLEl-GS's upstream tools enable push descriptors,
 * descriptor heaps and VK_EXT_descriptor_buffer. On lavapipe (llvmpipe,
 * Mesa 25.0.7) the descriptor-buffer path crashes inside the driver's
 * compute thread pool (glibc "pthread_mutex_lock ... __owner == 0"
 * assertion; reproduced with the submodule's own parallel-gs-sandbox, so it
 * is a driver bug, not a stream bug). CPU-type devices therefore get the
 * context re-created without the descriptor-buffer bit. ICORECOMP_GS_NO_DESCBUF=1
 * forces the bit off on any device.
 *
 * Caller must have run Vulkan::Context::init_loader first.
 */
#ifndef ICORECOMP_GS_PGS_CONTEXT_H
#define ICORECOMP_GS_PGS_CONTEXT_H

#include "context.hpp"

#include <cstdlib>
#include <memory>

struct RtGsContextResult {
    std::unique_ptr<Vulkan::Context> context;
    bool descriptor_buffer_disabled = false;
};

inline RtGsContextResult rt_gs_make_pgs_context() {
    constexpr Vulkan::ContextCreationFlags kFull =
        Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
        Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
        Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;

    RtGsContextResult r;
    Vulkan::ContextCreationFlags flags = kFull;
    if (std::getenv("ICORECOMP_GS_NO_DESCBUF")) {
        flags &= ~Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;
        r.descriptor_buffer_disabled = true;
    }

    for (;;) {
        auto ctx = std::make_unique<Vulkan::Context>();
        ctx->set_num_thread_indices(1);
        if (!ctx->init_instance_and_device(nullptr, 0, nullptr, 0, flags)) {
            return {}; /* context == nullptr: no usable device */
        }
        const bool cpu_device =
            ctx->get_gpu_props().deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        if (cpu_device && (flags & Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT)) {
            /* Recreate without the broken path; see the header comment. */
            flags &= ~Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;
            r.descriptor_buffer_disabled = true;
            continue;
        }
        r.context = std::move(ctx);
        return r;
    }
}

#endif /* ICORECOMP_GS_PGS_CONTEXT_H */
