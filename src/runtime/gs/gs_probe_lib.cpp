/* gs/gs_probe_lib.cpp: rt_pgs_probe, the device-capability probe.
 *
 * Compiles into libicorecomp-parallel-gs, so it may use the Granite C++
 * interfaces; the executable side sees only the POD in gs_probe_api.h.
 *
 * It answers one question and nothing else: on this machine, does a Vulkan
 * device enumerate, and does it meet every requirement paraLLEl-GS checks
 * in GSRenderer::init (third_party/parallel-gs/gs/gs_renderer.cpp:800-831)?
 * The requirement list here is a transcription of that check, feature for
 * feature, so a machine that fails reports which line it failed rather than
 * the single "Minimum requirements for parallel-gs are not met" the
 * renderer prints. This exists because the macOS build ships untested: the
 * first thing a Mac user or a CI runner can report back is this struct.
 *
 * The device is created exactly the way the live backend's headless path
 * creates it (gs_pgs_context.h then Vulkan::Device), so a pass here means
 * the same device the backend would have used passed.
 *
 * Ours (MIT).
 */
#include "gs_probe_api.h"

#include "gs_parallel_impl.h"
#include "gs_pgs_context.h"

#include "context.hpp"
#include "device.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>

namespace {

/* The subgroup operations gs_renderer.cpp requires, in one constant so the
 * pass/fail field and the raw flags reported alongside it cannot drift. */
constexpr VkSubgroupFeatureFlags kRequiredSubgroupFlags =
    VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
    VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
    VK_SUBGROUP_FEATURE_VOTE_BIT |
    VK_SUBGROUP_FEATURE_BALLOT_BIT |
    VK_SUBGROUP_FEATURE_BASIC_BIT;

/* Everything RtPgsProbe holds, read off a device that already exists. Shared
 * by rt_pgs_probe, which makes a device of its own, and rt_pgs_live_probe,
 * which uses the live one, so the two answers cannot drift apart. */
void fill_from_device(Vulkan::Device& device, RtPgsProbe* out) {
    const auto& props = device.get_gpu_properties();
    const auto& ext = device.get_device_features();

    out->have_device = 1;
    std::snprintf(out->device_name, sizeof(out->device_name), "%s", props.deviceName);
    out->api_version = props.apiVersion;
    out->driver_version = props.driverVersion;
    out->vendor_id = props.vendorID;
    out->device_id = props.deviceID;
    out->device_type = uint32_t(props.deviceType);
    out->max_compute_shared_memory = props.limits.maxComputeSharedMemorySize;
    out->subgroup_supported_ops = ext.vk11_props.subgroupSupportedOperations;

    out->descriptor_indexing = ext.vk12_features.descriptorIndexing ? 1 : 0;
    out->timeline_semaphore = ext.vk12_features.timelineSemaphore ? 1 : 0;
    out->buffer_device_address = ext.vk12_features.bufferDeviceAddress ? 1 : 0;
    out->storage_buffer_8bit = ext.vk12_features.storageBuffer8BitAccess ? 1 : 0;
    out->storage_buffer_16bit = ext.vk11_features.storageBuffer16BitAccess ? 1 : 0;
    out->shader_int16 = ext.enabled_features.shaderInt16 ? 1 : 0;
    out->scalar_block_layout = ext.vk12_features.scalarBlockLayout ? 1 : 0;
    out->subgroup_ops =
        (ext.vk11_props.subgroupSupportedOperations & kRequiredSubgroupFlags)
            == kRequiredSubgroupFlags ? 1 : 0;
    /* full-group compute, 2^2 to 2^6 invocations: the same call and the
     * same arguments GSRenderer::init makes. */
    out->subgroup_size_control = device.supports_subgroup_size_log2(true, 2, 6) ? 1 : 0;
    out->compute_shared_memory = props.limits.maxComputeSharedMemorySize >= 32 * 1024 ? 1 : 0;

    out->all_pass =
        out->descriptor_indexing && out->timeline_semaphore &&
        out->buffer_device_address && out->storage_buffer_8bit &&
        out->storage_buffer_16bit && out->shader_int16 &&
        out->scalar_block_layout && out->subgroup_ops &&
        out->subgroup_size_control && out->compute_shared_memory;
}

} // namespace

extern "C" RT_GS_API int32_t rt_pgs_live_probe(void* pgs, RtPgsProbe* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));
    RtPgs* inst = static_cast<RtPgs*>(pgs);
    if (!inst) return 0;
    Vulkan::Device* device = inst->live_device();
    if (!device) return 0;
    fill_from_device(*device, out);
    /* Not readable off the device: only the instance that created the
     * context knows whether the bit was dropped. Without this the Display
     * tab's feature line could never report the one fact gs_pgs_context.h's
     * fallbacks exist to make visible. */
    out->descriptor_buffer_disabled = inst->descriptor_buffer_disabled() ? 1 : 0;
    return 1;
}

extern "C" RT_GS_API int32_t rt_pgs_probe(RtPgsProbe* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));

    /* Granite's Vulkan entry points throw on several failure paths, and a
     * probe that terminates the process would defeat its own purpose: "no
     * device" has to come back as a value. */
    try {
        if (!Vulkan::Context::init_loader(nullptr)) {
            std::fprintf(stderr, "paraLLEl-GS probe: no Vulkan loader "
                                 "(no libvulkan/vulkan-1.dll, or no ICD installed)\n");
            return 0;
        }

        RtGsContextResult ctx = rt_gs_make_pgs_context();
        if (!ctx.context) {
            std::fprintf(stderr, "paraLLEl-GS probe: no Vulkan device enumerated\n");
            return 0;
        }
        out->descriptor_buffer_disabled = ctx.descriptor_buffer_disabled ? 1 : 0;

        auto device = std::make_unique<Vulkan::Device>();
        device->set_context(*ctx.context);
        /* One frame context: nothing is ever submitted here, but the device
         * wants its per-frame state before any query touches it. */
        device->init_frame_contexts(1);

        fill_from_device(*device, out);

        /* The Device holds the last reference to the queues; destroying it
         * before the Context is what a clean teardown needs. */
        device.reset();
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "paraLLEl-GS probe: device creation threw: %s\n", e.what());
        std::memset(out, 0, sizeof(*out));
        return 0;
    } catch (...) {
        std::fprintf(stderr, "paraLLEl-GS probe: device creation threw an unknown exception\n");
        std::memset(out, 0, sizeof(*out));
        return 0;
    }
}
