/* rhi/vulkan/rhi_vulkan_device.cpp: device, resources, pipelines, swapchain.
 *
 * Ours (MIT). See rhi_vulkan.h for the shape of the backend and rhi.h for the
 * interface it implements.
 */
#include "rhi_vulkan.h"

#include "../../runtime.h"

#include <cstdarg>
#include <cstdio>

namespace rhi {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (!data || !data->pMessage) return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        rt_log_error("rhi", "validation: %s", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        rt_log_warn("rhi", "validation: %s", data->pMessage);
    } else {
        rt_log_debug("rhi", "validation: %s", data->pMessage);
    }
    return VK_FALSE;
}

/* Cuts a two-call enumeration's vector down to what the second call actually
 * wrote into it.
 *
 * Every vkEnumerate* and vkGet*Properties pair in this file sizes its vector
 * from the count the first call reported and then passes that count in again.
 * The spec has the second call write the number of entries it filled back
 * through the same pointer, and that number is allowed to be smaller: the set
 * can shrink between the two calls, and a driver may fill in fewer than it
 * counted. The tail of the vector is then default-constructed handles and
 * uninitialised property bytes, which every caller here goes on to walk with
 * a range-for. `written` is the count the second call left behind.
 *
 * A second call that failed outright leaves the whole buffer's contents
 * undefined, so that clears the vector rather than trimming it. VK_INCOMPLETE
 * is not a failure: it means the buffer was filled to the brim and the caller
 * has already said so in its log. */
template <typename T>
void shrink_to_written(std::vector<T>& v, uint32_t written, VkResult result) {
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        v.clear();
        return;
    }
    if (written < v.size()) v.resize(written);
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const VkExtensionProperties& e : list) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

/* The results the calls in this file can actually return, by name. A bare
 * number in a log is a header lookup away from meaning anything, and every
 * name below was taken from third_party/Vulkan-Headers rather than recalled,
 * which is also why an unlisted code is reported as unrecognised instead of
 * being guessed at. */
const char* vk_result_name(VkResult r) {
    switch (r) {
        case VK_SUCCESS:                            return "VK_SUCCESS";
        case VK_NOT_READY:                          return "VK_NOT_READY";
        case VK_TIMEOUT:                            return "VK_TIMEOUT";
        case VK_INCOMPLETE:                         return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR:                     return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:           return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:         return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:        return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:                  return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:            return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_EXTENSION_NOT_PRESENT:        return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:          return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:          return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FRAGMENTED_POOL:              return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY:           return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_UNKNOWN:                      return "VK_ERROR_UNKNOWN";
        case VK_ERROR_SURFACE_LOST_KHR:             return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:     return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:              return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
            return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
        default: break;
    }
    return "unrecognized VkResult";
}

VkPresentModeKHR to_vk_present(PresentMode m) {
    switch (m) {
        case PresentMode::Mailbox:   return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
        default:                     return VK_PRESENT_MODE_FIFO_KHR;
    }
}

} // namespace

VkFormat to_vk_format(Format f) {
    switch (f) {
        case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::R32Uint:    return VK_FORMAT_R32_UINT;
        default: break;
    }
    /* VK_FORMAT_UNDEFINED is what the caller gets, and vkCreateImage or
     * vkCreateGraphicsPipelines then fails with a message about the image or
     * the pipeline. The format that was actually asked for is only here. */
    rt_log_error("rhi", "rhi::Format %u has no Vulkan format on this backend; the caller is "
                        "given VK_FORMAT_UNDEFINED and whatever it creates will fail",
                 (unsigned)f);
    return VK_FORMAT_UNDEFINED;
}

void VulkanDevice::fatal(const char* fmt, ...) const {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    /* The device name is in every fatal on purpose: the same message from two
     * machines is the same bug only if it is the same driver. */
    rt_fatal("rhi", nullptr, "%s (device: %s, %s)", msg, m_device_name.c_str(),
             m_api_version.empty() ? "no API version yet" : m_api_version.c_str());
}

void VulkanDevice::check(VkResult r, const char* what) const {
    if (r != VK_SUCCESS) fatal("%s failed with VkResult %d (%s)", what, (int)r, vk_result_name(r));
}

/* A field that reaches no screen. Counted rather than printed: a minimised
 * window or a swapchain being rebuilt would otherwise write one line every
 * field, which buries the reason it started. The first field of a stall says
 * why, at warn; note_presentation_resumed says how many there were. */
void VulkanDevice::note_present_stall(const char* fmt, ...) {
    ++m_stalled_fields;
    if (m_present_stalled) return;
    m_present_stalled = true;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    rt_log_warn("rhi", "nothing is being presented: %s. Later fields are counted, not logged; "
                       "the line that says presentation resumed carries the total.", msg);
}

void VulkanDevice::note_presentation_resumed() {
    if (!m_present_stalled) return;
    m_present_stalled = false;
    rt_log_info("rhi", "presentation resumed; %llu field(s) reached no screen while it was "
                       "stalled", (unsigned long long)m_stalled_fields);
    m_stalled_fields = 0;
}

/* ---- construction --------------------------------------------------------- */

VulkanDevice::VulkanDevice(const DeviceDesc& desc) {
    m_validation = desc.validation;
    m_present_mode = desc.present_mode;

    const VkResult volk_result = volkInitialize();
    if (volk_result != VK_SUCCESS) {
        rt_fatal("rhi", nullptr,
                 "no Vulkan loader was found on this system (volkInitialize: VkResult %d, %s); "
                 "the native GS renderer cannot start. Install the GPU vendor's driver, or run "
                 "with the paraLLEl-GS backend (ICORECOMP_GS=parallel).",
                 (int)volk_result, vk_result_name(volk_result));
    }

    create_instance(desc);
    if (!desc.headless) create_surface(desc);
    pick_physical_device(desc.prefer_software);
    create_device();
    create_layout_and_samplers();
    create_frames();
    /* Before create_dummies, which records and submits. */
    m_cmd = new VulkanCommandList(this);
    create_dummies();
    if (m_surface != VK_NULL_HANDLE) {
        create_swapchain(desc.surface_width, desc.surface_height);
    }

    rt_log_info("rhi", "Vulkan device: %s (%s)%s%s%s", m_device_name.c_str(),
                m_api_version.c_str(),
                m_swapchain != VK_NULL_HANDLE ? ", swapchain" : ", headless",
                m_validation ? ", validation layers on" : "",
                m_software ? ", software rasteriser" : "");
}

VulkanDevice::~VulkanDevice() {
    if (m_device == VK_NULL_HANDLE) return;
    /* Not a fatal, unlike every other vkDeviceWaitIdle here: destroying the
     * device is what the spec prescribes after a lost one, and this is that
     * teardown. Ending the process from a destructor would also report a run
     * that had already finished as a failure. Loud at error either way, since
     * everything destroyed below is being destroyed while work may still be
     * in flight. */
    const VkResult idle = vkDeviceWaitIdle(m_device);
    if (idle != VK_SUCCESS) {
        rt_log_error("rhi", "vkDeviceWaitIdle during device teardown failed with VkResult %d "
                            "(%s); the objects below are destroyed anyway, which is what the "
                            "spec asks for after a lost device", (int)idle, vk_result_name(idle));
    }
    delete m_cmd;
    /* After the vkDeviceWaitIdle above; destroy_swapchain frees the retired
     * generation too, which is certainly finished with by then. */
    destroy_swapchain();
    if (m_dummy_buffer) destroy_buffer(m_dummy_buffer);
    if (m_dummy_texture) destroy_texture(m_dummy_texture);
    if (m_dummy_storage_image) destroy_texture(m_dummy_storage_image);
    for (Frame& f : m_frames) {
        if (f.pool) vkDestroyCommandPool(m_device, f.pool, nullptr);
        if (f.descriptors) vkDestroyDescriptorPool(m_device, f.descriptors, nullptr);
        if (f.acquire) vkDestroySemaphore(m_device, f.acquire, nullptr);
    }
    if (m_timeline) vkDestroySemaphore(m_device, m_timeline, nullptr);
    for (VkSampler s : m_samplers) {
        if (s) vkDestroySampler(m_device, s, nullptr);
    }
    if (m_pipeline_cache) vkDestroyPipelineCache(m_device, m_pipeline_cache, nullptr);
    if (m_pipeline_layout) vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
    if (m_set_layout) vkDestroyDescriptorSetLayout(m_device, m_set_layout, nullptr);
    vkDestroyDevice(m_device, nullptr);
    if (m_surface) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_messenger) vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
    vkDestroyInstance(m_instance, nullptr);
}

void VulkanDevice::create_instance(const DeviceDesc& desc) {
    /* The loader's own version, before anything asks it for a 1.3 instance.
     * vkCreateInstance with an apiVersion the loader does not know returns
     * VK_ERROR_INCOMPATIBLE_DRIVER, and that number on its own does not say
     * which side is old. vkEnumerateInstanceVersion is a 1.1 entry point; a
     * loader that does not export it is 1.0. */
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loader_version);
    if (loader_version < VK_API_VERSION_1_3) {
        rt_fatal("rhi", nullptr,
                 "the Vulkan loader on this system reports %u.%u.%u and this renderer "
                 "needs 1.3 (dynamic rendering, synchronization2, timeline semaphores). "
                 "Update the GPU vendor's driver, or run with ICORECOMP_GS_BACKEND=d3d12 or "
                 "the paraLLEl-GS backend (ICORECOMP_GS=parallel).",
                 VK_API_VERSION_MAJOR(loader_version), VK_API_VERSION_MINOR(loader_version),
                 VK_API_VERSION_PATCH(loader_version));
    }

    /* Both calls are checked because the require() below turns a short list
     * into "the loader offers no VK_KHR_surface", which names the wrong
     * problem when what actually happened is that the enumeration failed. */
    uint32_t count = 0;
    VkResult ext_result = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    if (ext_result != VK_SUCCESS) {
        rt_log_warn("rhi", "vkEnumerateInstanceExtensionProperties (count) returned VkResult %d "
                           "(%s); an extension that is present may be reported as missing below",
                    (int)ext_result, vk_result_name(ext_result));
    }
    std::vector<VkExtensionProperties> available(count);
    if (count) {
        ext_result = vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        if (ext_result != VK_SUCCESS) {
            rt_log_warn("rhi", "vkEnumerateInstanceExtensionProperties returned VkResult %d "
                               "(%s) for %u extensions; an extension that is present may be "
                               "reported as missing below",
                        (int)ext_result, vk_result_name(ext_result), count);
        }
        /* The second call writes back how many it actually filled, which the
         * spec allows to be fewer than the first call reported. Everything
         * past that point in the vector was never written, so the list is cut
         * to what the driver wrote; has_extension below walks the whole
         * vector and would otherwise strcmp against uninitialised bytes. */
        shrink_to_written(available, count, ext_result);
    }

    std::vector<const char*> extensions;
    std::vector<const char*> layers;

    /* Each surface extension is checked before it is asked for. Without this
     * the failure is one VkResult from vkCreateInstance with no name in it,
     * and the name is the whole answer: a machine with no window system
     * integration for the platform it is on cannot present, and that is a
     * different problem from a driver that is too old. */
    auto require = [&](const char* name) {
        if (!has_extension(available, name)) {
            rt_fatal("rhi", nullptr,
                     "the Vulkan loader offers no %s, so this window cannot be presented "
                     "to. Update the GPU vendor's driver, or run with "
                     "ICORECOMP_GS_BACKEND=d3d12 or the paraLLEl-GS backend "
                     "(ICORECOMP_GS=parallel).", name);
        }
        extensions.push_back(name);
    };

    if (!desc.headless) {
        require(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(ICORECOMP_RHI_WIN32)
        if (desc.win32_hwnd) require(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
#if defined(ICORECOMP_RHI_XLIB)
        if (desc.x11_display) require(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
#if defined(ICORECOMP_RHI_WAYLAND)
        if (desc.wl_display) require(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif
    }
    if (m_validation) {
        /* Both halves have to be there. VK_EXT_debug_utils is the messenger,
         * and VK_LAYER_KHRONOS_validation is what produces the messages; a
         * layer named in VkInstanceCreateInfo that is not installed fails
         * vkCreateInstance outright, which used to turn a verbose run on a
         * machine without the Vulkan SDK into a fatal at startup. */
        uint32_t layer_count = 0;
        VkResult layer_result = vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        if (layer_result != VK_SUCCESS) {
            rt_log_warn("rhi", "vkEnumerateInstanceLayerProperties (count) returned VkResult %d "
                               "(%s); validation may be reported as absent when it is installed",
                        (int)layer_result, vk_result_name(layer_result));
        }
        std::vector<VkLayerProperties> layer_props(layer_count);
        if (layer_count) {
            layer_result = vkEnumerateInstanceLayerProperties(&layer_count, layer_props.data());
            if (layer_result != VK_SUCCESS) {
                rt_log_warn("rhi", "vkEnumerateInstanceLayerProperties returned VkResult %d (%s) "
                                   "for %u layers; validation may be reported as absent when it "
                                   "is installed",
                            (int)layer_result, vk_result_name(layer_result), layer_count);
            }
            shrink_to_written(layer_props, layer_count, layer_result);
        }
        bool have_layer = false;
        for (const VkLayerProperties& l : layer_props) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                have_layer = true;
                break;
            }
        }
        const bool have_debug_utils = has_extension(available,
                                                    VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (have_layer && have_debug_utils) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            layers.push_back("VK_LAYER_KHRONOS_validation");
            rt_log_info("rhi", "Vulkan validation is on (built with "
                               "ICORECOMP_RHI_VALIDATION); VK_LAYER_KHRONOS_validation and "
                               "VK_EXT_debug_utils are both present");
        } else {
            rt_log_warn("rhi", "validation was asked for (built with "
                               "ICORECOMP_RHI_VALIDATION) and this system has %s; continuing without it",
                        !have_layer ? "no VK_LAYER_KHRONOS_validation (install the Vulkan "
                                      "SDK or the validation layers package)"
                                    : "no VK_EXT_debug_utils");
            m_validation = false;
        }
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "icorecomp";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)extensions.size();
    ci.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    const VkResult r = vkCreateInstance(&ci, nullptr, &m_instance);
    if (r != VK_SUCCESS) {
        rt_fatal("rhi", nullptr,
                 "vkCreateInstance failed with VkResult %d (%s). The loader is present but no "
                 "Vulkan 1.3 instance could be created.", (int)r, vk_result_name(r));
    }
    volkLoadInstance(m_instance);

    if (m_validation) {
        VkDebugUtilsMessengerCreateInfoEXT dm{};
        dm.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dm.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                           | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dm.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dm.pfnUserCallback = debug_callback;
        /* Without this the layer still runs, but nothing it says reaches this
         * log, which reads exactly like a validation run with no findings. */
        const VkResult mr = vkCreateDebugUtilsMessengerEXT(m_instance, &dm, nullptr, &m_messenger);
        if (mr != VK_SUCCESS) {
            m_messenger = VK_NULL_HANDLE;
            rt_log_warn("rhi", "vkCreateDebugUtilsMessengerEXT failed with VkResult %d (%s); "
                               "validation is enabled and none of its messages will appear in "
                               "this log", (int)mr, vk_result_name(mr));
        }
    }
}

void VulkanDevice::create_surface(const DeviceDesc& desc) {
#if defined(ICORECOMP_RHI_WIN32)
    if (desc.win32_hwnd) {
        VkWin32SurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        ci.hinstance = (HINSTANCE)desc.win32_hinstance;
        ci.hwnd = (HWND)desc.win32_hwnd;
        check(vkCreateWin32SurfaceKHR(m_instance, &ci, nullptr, &m_surface),
              "vkCreateWin32SurfaceKHR");
        return;
    }
#endif
#if defined(ICORECOMP_RHI_XLIB)
    if (desc.x11_display) {
        VkXlibSurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        ci.dpy = (Display*)desc.x11_display;
        ci.window = (::Window)desc.x11_window;
        check(vkCreateXlibSurfaceKHR(m_instance, &ci, nullptr, &m_surface),
              "vkCreateXlibSurfaceKHR");
        return;
    }
#endif
#if defined(ICORECOMP_RHI_WAYLAND)
    if (desc.wl_display) {
        VkWaylandSurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        ci.display = (struct wl_display*)desc.wl_display;
        ci.surface = (struct wl_surface*)desc.wl_surface;
        check(vkCreateWaylandSurfaceKHR(m_instance, &ci, nullptr, &m_surface),
              "vkCreateWaylandSurfaceKHR");
        return;
    }
#endif
    (void)desc;
    /* A window was asked for and this build has no surface path for the
     * platform it is on. Loud, because the alternative is a run that draws
     * into nothing and looks like a hang. */
    rt_fatal("rhi", nullptr,
             "a window was requested but this build has no Vulkan surface support for "
             "the platform handles it was given (win32=%p x11=%p wayland=%p)",
             desc.win32_hwnd, desc.x11_display, desc.wl_display);
}

void VulkanDevice::pick_physical_device(bool prefer_software) {
    uint32_t count = 0;
    VkResult enum_result = vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (enum_result != VK_SUCCESS && enum_result != VK_INCOMPLETE) {
        rt_fatal("rhi", nullptr,
                 "vkEnumeratePhysicalDevices (count) failed with VkResult %d (%s), so no device "
                 "can be picked", (int)enum_result, vk_result_name(enum_result));
    }
    if (count == 0) {
        rt_fatal("rhi", nullptr, "no Vulkan physical device was enumerated");
    }
    std::vector<VkPhysicalDevice> gpus(count);
    enum_result = vkEnumeratePhysicalDevices(m_instance, &count, gpus.data());
    if (enum_result != VK_SUCCESS && enum_result != VK_INCOMPLETE) {
        rt_fatal("rhi", nullptr,
                 "vkEnumeratePhysicalDevices failed with VkResult %d (%s) for %u devices, so no "
                 "device can be picked", (int)enum_result, vk_result_name(enum_result), count);
    }
    if (enum_result == VK_INCOMPLETE) {
        rt_log_warn("rhi", "vkEnumeratePhysicalDevices returned VkResult %d (%s): the device "
                           "list grew between the two calls and only %u are considered",
                    (int)enum_result, vk_result_name(enum_result), count);
    }
    /* Cut to what the second call wrote. The loop below is a range-for over
     * the vector, so a driver that reported more devices than it filled would
     * otherwise hand vkGetPhysicalDeviceProperties an uninitialised handle. */
    shrink_to_written(gpus, count, enum_result);
    if (gpus.empty()) {
        rt_fatal("rhi", nullptr,
                 "vkEnumeratePhysicalDevices reported %u devices and then filled none in",
                 count);
    }

    /* Prefer a discrete GPU, then anything that meets the requirements. The
     * first pass records why each rejected device was rejected, so a machine
     * that ends up on the software rasteriser says so.
     *
     * DeviceDesc::prefer_software inverts that: only a CPU implementation
     * (lavapipe, SwiftShader) is accepted, and a machine with no such device
     * is a fatal rather than a quiet fall back onto the GPU. That is what
     * makes the flag mean the same thing here as it does on D3D12, where it
     * selects WARP; the CI job that runs these shaders on a GitHub runner
     * depends on the picture coming from the software rasteriser and not from
     * whatever the loader happens to rank first. */
    VkPhysicalDevice best = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties best_props{};
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) {
            rt_log_warn("rhi", "skipping %s: Vulkan %u.%u, and this renderer needs 1.3",
                        props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                        VK_API_VERSION_MINOR(props.apiVersion));
            continue;
        }
        if (m_surface != VK_NULL_HANDLE) {
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
            bool can_present = false;
            for (uint32_t i = 0; i < families; ++i) {
                VkBool32 supported = VK_FALSE;
                /* A query that fails leaves supported false, and the device is
                 * then skipped for a reason it may not have. Both the skip and
                 * the fatal that follows when every device is skipped would
                 * otherwise blame the hardware for a failed call. */
                const VkResult sr = vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, m_surface,
                                                                         &supported);
                if (sr != VK_SUCCESS) {
                    rt_log_warn("rhi", "vkGetPhysicalDeviceSurfaceSupportKHR(%s, queue family "
                                       "%u) returned VkResult %d (%s); that family is treated "
                                       "as unable to present", props.deviceName, i, (int)sr,
                                vk_result_name(sr));
                    continue;
                }
                if (supported) { can_present = true; break; }
            }
            if (!can_present) {
                rt_log_warn("rhi", "skipping %s: it cannot present to this window's surface",
                            props.deviceName);
                continue;
            }
        }
        if (prefer_software && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            rt_log_warn("rhi", "skipping %s: a software device was asked for and this one "
                               "is not a CPU implementation", props.deviceName);
            continue;
        }
        const bool better = best == VK_NULL_HANDLE
            || (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                && best_props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (better) {
            best = gpu;
            best_props = props;
        }
    }
    if (best == VK_NULL_HANDLE && prefer_software) {
        rt_fatal("rhi", nullptr,
                 "a software Vulkan device was asked for and the loader enumerated none "
                 "(no VK_PHYSICAL_DEVICE_TYPE_CPU device). Install a software "
                 "implementation such as mesa's lavapipe. This is not falling back to a "
                 "GPU: a run that asked for the software rasteriser and got hardware "
                 "would be measuring the wrong thing");
    }
    if (best == VK_NULL_HANDLE) {
        rt_fatal("rhi", nullptr,
                 "no Vulkan device on this system meets the renderer's requirements "
                 "(Vulkan 1.3, and presentation to the window when there is one)");
    }
    m_software = best_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    m_gpu = best;
    m_device_name = best_props.deviceName;
    char v[64];
    std::snprintf(v, sizeof(v), "Vulkan %u.%u.%u",
                  VK_API_VERSION_MAJOR(best_props.apiVersion),
                  VK_API_VERSION_MINOR(best_props.apiVersion),
                  VK_API_VERSION_PATCH(best_props.apiVersion));
    m_api_version = v;
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &m_mem_props);

    /* What the renderer is allowed to ask this device for. Read here, from
     * the device that was just picked, rather than from the floor the
     * specification guarantees: gs/render/gs_native.cpp derives every compute
     * dispatch grid from a guest register and checks it against these three
     * numbers, and a device that offers more than 65535 on an axis should not
     * have a field refused for asking for it. */
    m_limits.max_workgroup_count[0] = best_props.limits.maxComputeWorkGroupCount[0];
    m_limits.max_workgroup_count[1] = best_props.limits.maxComputeWorkGroupCount[1];
    m_limits.max_workgroup_count[2] = best_props.limits.maxComputeWorkGroupCount[2];
    m_limits.frames_in_flight = kFrames;
    rt_log_debug("rhi", "device limits: compute workgroup grid %ux%ux%u, %u frames in flight",
                 m_limits.max_workgroup_count[0], m_limits.max_workgroup_count[1],
                 m_limits.max_workgroup_count[2], m_limits.frames_in_flight);
}

void VulkanDevice::create_device() {
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &families, props.data());
    /* This entry point writes back the count it filled as well, and the loop
     * below indexes props by it. */
    if (families < props.size()) props.resize(families);
    for (uint32_t i = 0; i < families; ++i) {
        const bool graphics = (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool compute = (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        if (!graphics || !compute) continue;
        if (m_surface != VK_NULL_HANDLE) {
            VkBool32 supported = VK_FALSE;
            const VkResult sr = vkGetPhysicalDeviceSurfaceSupportKHR(m_gpu, i, m_surface,
                                                                     &supported);
            if (sr != VK_SUCCESS) {
                /* Same as in pick_physical_device: without this the queue
                 * family is passed over and the fatal below names the device
                 * rather than the call that failed. */
                rt_log_warn("rhi", "vkGetPhysicalDeviceSurfaceSupportKHR(queue family %u) "
                                   "returned VkResult %d (%s); that family is treated as "
                                   "unable to present", i, (int)sr, vk_result_name(sr));
                continue;
            }
            if (!supported) continue;
        }
        m_queue_family = i;
        break;
    }
    if (m_queue_family == UINT32_MAX) {
        fatal("no queue family supports graphics, compute and presentation together");
    }

    /* Checked for the same reason the instance list is: a failed enumeration
     * reads below as a device without VK_KHR_swapchain, which is a different
     * and much more alarming fact. */
    uint32_t count = 0;
    VkResult ext_result = vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &count, nullptr);
    if (ext_result != VK_SUCCESS) {
        rt_log_warn("rhi", "vkEnumerateDeviceExtensionProperties (count) returned VkResult %d "
                           "(%s); a device extension that is present may be reported as missing",
                    (int)ext_result, vk_result_name(ext_result));
    }
    std::vector<VkExtensionProperties> available(count);
    if (count) {
        ext_result = vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &count,
                                                          available.data());
        if (ext_result != VK_SUCCESS) {
            rt_log_warn("rhi", "vkEnumerateDeviceExtensionProperties returned VkResult %d (%s) "
                               "for %u extensions; a device extension that is present may be "
                               "reported as missing",
                        (int)ext_result, vk_result_name(ext_result), count);
        }
        shrink_to_written(available, count, ext_result);
    }

    std::vector<const char*> extensions;
    if (m_surface != VK_NULL_HANDLE) {
        if (!has_extension(available, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            fatal("the device does not support VK_KHR_swapchain, so it cannot present");
        }
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(m_gpu, &f2);

    if (!f13.dynamicRendering) fatal("the device does not support dynamicRendering");
    if (!f13.synchronization2) fatal("the device does not support synchronization2");
    if (!f12.timelineSemaphore) fatal("the device does not support timelineSemaphore");

    /* Ask for exactly the three features that were checked, and nothing else.
     * A feature enabled but unused is a requirement this renderer would be
     * imposing on every backend and every device for no reason. */
    VkPhysicalDeviceVulkan13Features want13{};
    want13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    want13.dynamicRendering = VK_TRUE;
    want13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features want12{};
    want12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    want12.timelineSemaphore = VK_TRUE;
    want12.pNext = &want13;
    VkPhysicalDeviceFeatures2 want2{};
    want2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    want2.pNext = &want12;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = m_queue_family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &want2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = (uint32_t)extensions.size();
    ci.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    check(vkCreateDevice(m_gpu, &ci, nullptr, &m_device), "vkCreateDevice");
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo sem{};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem.pNext = &type;
    check(vkCreateSemaphore(m_device, &sem, nullptr, &m_timeline), "timeline semaphore");

    VkPipelineCacheCreateInfo pc{};
    pc.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    /* A null cache is legal in vkCreate*Pipelines, so this is a fallback and
     * not a stop: every pipeline is compiled from scratch instead. Named,
     * because the cost lands on startup time and nowhere else. */
    const VkResult cache_result = vkCreatePipelineCache(m_device, &pc, nullptr,
                                                        &m_pipeline_cache);
    if (cache_result != VK_SUCCESS) {
        m_pipeline_cache = VK_NULL_HANDLE;
        rt_log_warn("rhi", "vkCreatePipelineCache failed with VkResult %d (%s); pipelines are "
                           "built without a cache, which only costs compilation time",
                    (int)cache_result, vk_result_name(cache_result));
    }
}

void VulkanDevice::create_layout_and_samplers() {
    /* The four immutable samplers, in the order rhi.h documents. */
    const struct { VkFilter filter; VkSamplerAddressMode address; } kSpecs[kSamplerCount] = {
        { VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE },
        { VK_FILTER_LINEAR,  VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE },
        { VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT },
        { VK_FILTER_LINEAR,  VK_SAMPLER_ADDRESS_MODE_REPEAT },
    };
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = kSpecs[i].filter;
        si.minFilter = kSpecs[i].filter;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = kSpecs[i].address;
        si.addressModeV = kSpecs[i].address;
        si.addressModeW = kSpecs[i].address;
        si.maxLod = VK_LOD_CLAMP_NONE;
        check(vkCreateSampler(m_device, &si, nullptr, &m_samplers[i]), "vkCreateSampler");
    }

    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = kMaxUniformBuffers;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = kMaxStorageBuffers;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[2].descriptorCount = kMaxSampledTextures;
    bindings[2].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[3].descriptorCount = kSamplerCount;
    bindings[3].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[3].pImmutableSamplers = m_samplers;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[4].descriptorCount = kMaxStorageImages;
    bindings[4].stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 5;
    li.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_set_layout),
          "vkCreateDescriptorSetLayout");

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_ALL;
    range.offset = 0;
    range.size = kPushConstantBytes;

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_pipeline_layout),
          "vkCreatePipelineLayout");
}

void VulkanDevice::create_frames() {
    /* Sized for the renderer's worst frame: a scanout dispatch, a present
     * blit and one overlay draw per command, which is a few hundred sets at
     * most. A pool that runs out is fatal rather than grown, because a silent
     * growth here would hide a leak in the caller. */
    const VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 * 1024 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 * 1024 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 * 1024 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 4 * 1024 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * 1024 },
    };
    for (Frame& f : m_frames) {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = m_queue_family;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        check(vkCreateCommandPool(m_device, &pi, nullptr, &f.pool), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(m_device, &ai, &f.cb), "vkAllocateCommandBuffers");

        VkDescriptorPoolCreateInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        di.maxSets = 1024;
        di.poolSizeCount = (uint32_t)(sizeof(sizes) / sizeof(sizes[0]));
        di.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(m_device, &di, nullptr, &f.descriptors),
              "vkCreateDescriptorPool");

        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        check(vkCreateSemaphore(m_device, &si, nullptr, &f.acquire), "acquire semaphore");
    }
}

void VulkanDevice::create_dummies() {
    /* Every element of every binding array has to be a valid descriptor,
     * because this backend deliberately does not use partially bound
     * descriptor indexing. These fill the slots nothing bound. */
    BufferDesc bd;
    bd.size = 256;
    bd.kind = BufferKind::DeviceLocal;
    bd.usage = BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::CopyDst;
    bd.debug_name = "rhi dummy buffer";
    m_dummy_buffer = create_buffer(bd);

    TextureDesc td;
    td.width = 1;
    td.height = 1;
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::Sampled | TextureUsage::CopyDst;
    td.debug_name = "rhi dummy texture";
    m_dummy_texture = create_texture(td);

    td.usage = TextureUsage::Storage;
    td.debug_name = "rhi dummy storage image";
    m_dummy_storage_image = create_texture(td);

    /* Both dummies must be in a legal layout before anything reads them. One
     * submit at startup costs nothing and removes a whole class of validation
     * noise. */
    CommandList* cmd = begin_command_list();
    cmd->texture_barrier(m_dummy_texture, Stage::Host, Access::Write,
                         Stage::Graphics, Access::Read);
    cmd->texture_barrier(m_dummy_storage_image, Stage::Host, Access::Write,
                         Stage::Compute, Access::ReadWrite);
    wait(submit(cmd));
}

uint32_t VulkanDevice::memory_type(uint32_t bits, VkMemoryPropertyFlags props) const {
    for (uint32_t i = 0; i < m_mem_props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) == 0) continue;
        if ((m_mem_props.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

/* ---- buffers -------------------------------------------------------------- */

Buffer* VulkanDevice::create_buffer(const BufferDesc& desc) {
    if (desc.size == 0) fatal("create_buffer with size 0 (%s)",
                              desc.debug_name ? desc.debug_name : "unnamed");
    Buffer* b = new Buffer();
    b->size = desc.size;
    b->kind = desc.kind;

    VkBufferUsageFlags usage = 0;
    if (has(desc.usage, BufferUsage::Storage))  usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (has(desc.usage, BufferUsage::Uniform))  usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (has(desc.usage, BufferUsage::Vertex))   usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (has(desc.usage, BufferUsage::Index))    usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (has(desc.usage, BufferUsage::Indirect)) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (has(desc.usage, BufferUsage::CopySrc))  usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (has(desc.usage, BufferUsage::CopyDst))  usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = desc.size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(m_device, &ci, nullptr, &b->buffer), "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, b->buffer, &req);

    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (desc.kind == BufferKind::Upload) {
        want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    } else if (desc.kind == BufferKind::Readback) {
        want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
             | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    uint32_t type = memory_type(req.memoryTypeBits, want);
    if (type == UINT32_MAX && desc.kind == BufferKind::Readback) {
        /* Cached host memory is an optimisation, not a requirement. Said out
         * loud all the same: a readback out of uncached memory is slow enough
         * to explain a screenshot or a parity dump that takes far longer than
         * the same code on another machine. */
        type = memory_type(req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        rt_log_warn("rhi", "no host-cached memory type for the %llu byte readback buffer (%s); "
                           "using uncached host-visible memory, which reads back slowly",
                    (unsigned long long)desc.size,
                    desc.debug_name ? desc.debug_name : "unnamed");
    }
    if (type == UINT32_MAX) {
        fatal("no memory type for a %llu byte %s buffer (%s)",
              (unsigned long long)desc.size,
              desc.kind == BufferKind::DeviceLocal ? "device local"
                : desc.kind == BufferKind::Upload ? "upload" : "readback",
              desc.debug_name ? desc.debug_name : "unnamed");
    }

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    check(vkAllocateMemory(m_device, &ai, nullptr, &b->memory), "vkAllocateMemory (buffer)");
    check(vkBindBufferMemory(m_device, b->buffer, b->memory, 0), "vkBindBufferMemory");

    if (desc.kind != BufferKind::DeviceLocal) {
        check(vkMapMemory(m_device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped), "vkMapMemory");
    }
    return b;
}

void VulkanDevice::destroy_buffer(Buffer* b) {
    if (!b) return;
    if (b->mapped) vkUnmapMemory(m_device, b->memory);
    if (b->buffer) vkDestroyBuffer(m_device, b->buffer, nullptr);
    if (b->memory) vkFreeMemory(m_device, b->memory, nullptr);
    delete b;
}

void* VulkanDevice::map(Buffer* b) {
    if (!b) fatal("map of a null buffer");
    if (!b->mapped) fatal("map of a device-local buffer; only Upload and Readback are mappable");
    return b->mapped;
}

/* ---- textures ------------------------------------------------------------- */

Texture* VulkanDevice::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        fatal("create_texture %ux%u (%s)", desc.width, desc.height,
              desc.debug_name ? desc.debug_name : "unnamed");
    }
    Texture* t = new Texture();
    t->width = desc.width;
    t->height = desc.height;
    t->format = to_vk_format(desc.format);

    VkImageUsageFlags usage = 0;
    if (has(desc.usage, TextureUsage::Sampled))     usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has(desc.usage, TextureUsage::Storage))     usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (has(desc.usage, TextureUsage::ColorTarget)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has(desc.usage, TextureUsage::CopySrc))     usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has(desc.usage, TextureUsage::CopyDst))     usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = t->format;
    ci.extent = { desc.width, desc.height, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(m_device, &ci, nullptr, &t->image), "vkCreateImage");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, t->image, &req);
    const uint32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        fatal("no device-local memory type for a %ux%u image (%s)", desc.width, desc.height,
              desc.debug_name ? desc.debug_name : "unnamed");
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    check(vkAllocateMemory(m_device, &ai, nullptr, &t->memory), "vkAllocateMemory (image)");
    check(vkBindImageMemory(m_device, t->image, t->memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t->format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    check(vkCreateImageView(m_device, &vi, nullptr, &t->view), "vkCreateImageView");
    return t;
}

Format VulkanDevice::texture_format(Texture* t) const {
    if (!t) return Format::Unknown;
    switch (t->format) {
        case VK_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8Unorm;
        case VK_FORMAT_B8G8R8A8_UNORM: return Format::BGRA8Unorm;
        case VK_FORMAT_R32_UINT:       return Format::R32Uint;
        default: break;
    }
    /* Format::Unknown travels back through to_vk_format as
     * VK_FORMAT_UNDEFINED, so a texture whose format is outside rhi::Format
     * fails later at whatever it is handed to. Named here, where the actual
     * VkFormat is still known. */
    rt_log_error("rhi", "a texture has VkFormat %d, which rhi::Format does not carry; the "
                        "caller is told Format::Unknown", (int)t->format);
    return Format::Unknown;
}

void VulkanDevice::destroy_texture(Texture* t) {
    if (!t) return;
    if (t->view) vkDestroyImageView(m_device, t->view, nullptr);
    if (t->owns_image) {
        if (t->image) vkDestroyImage(m_device, t->image, nullptr);
        if (t->memory) vkFreeMemory(m_device, t->memory, nullptr);
    }
    delete t;
}

/* ---- pipelines ------------------------------------------------------------ */

namespace {

VkShaderModule make_module(const VulkanDevice* dev, VkDevice device,
                           const uint32_t* spirv, size_t words, const char* name) {
    if (!spirv || words == 0) dev->fatal("shader %s has no SPIR-V", name ? name : "(unnamed)");
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words * sizeof(uint32_t);
    ci.pCode = spirv;
    VkShaderModule m = VK_NULL_HANDLE;
    const VkResult r = vkCreateShaderModule(device, &ci, nullptr, &m);
    if (r != VK_SUCCESS) {
        dev->fatal("vkCreateShaderModule failed for %s with VkResult %d (%s)",
                   name ? name : "(unnamed)", (int)r, vk_result_name(r));
    }
    return m;
}

} // namespace

ComputePipeline* VulkanDevice::create_compute_pipeline(const uint32_t* spirv, size_t words,
                                                       const char* name) {
    VkShaderModule module = make_module(this, m_device, spirv, words, name);
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = m_pipeline_layout;

    ComputePipeline* p = new ComputePipeline();
    const VkResult r = vkCreateComputePipelines(m_device, m_pipeline_cache, 1, &ci, nullptr,
                                                &p->pipeline);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (r != VK_SUCCESS) {
        delete p;
        fatal("compute pipeline %s failed to build (VkResult %d, %s)",
              name ? name : "(unnamed)", (int)r, vk_result_name(r));
    }
    return p;
}

void VulkanDevice::destroy_compute_pipeline(ComputePipeline* p) {
    if (!p) return;
    if (p->pipeline) vkDestroyPipeline(m_device, p->pipeline, nullptr);
    delete p;
}

GraphicsPipeline* VulkanDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    const char* name = desc.debug_name ? desc.debug_name : "(unnamed)";
    VkShaderModule vs = make_module(this, m_device, desc.vertex_spirv,
                                    desc.vertex_spirv_words, name);
    VkShaderModule fs = make_module(this, m_device, desc.fragment_spirv,
                                    desc.fragment_spirv_words, name);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    /* The one vertex layout, documented in rhi.h: float2 position, float2
     * texture coordinate, one packed RGBA8 colour. 20 bytes. */
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 20;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 };
    attrs[2] = { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16 };

    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = 3;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                         | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = desc.blend ? VK_TRUE : VK_FALSE;
    /* Premultiplied geometry blends with ONE; straight alpha with SRC_ALPHA.
     * The UI emits the former, the ICORECOMP_UI_TEST frame the latter; see
     * RT_PGS_OVERLAY_PREMULTIPLIED in gs/gs_parallel_api.h. */
    blend.srcColorBlendFactor = desc.premultiplied ? VK_BLEND_FACTOR_ONE
                                                   : VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    const VkFormat color_format = to_vk_format(desc.color_format);
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vin;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dyn;
    ci.layout = m_pipeline_layout;

    GraphicsPipeline* p = new GraphicsPipeline();
    const VkResult r = vkCreateGraphicsPipelines(m_device, m_pipeline_cache, 1, &ci, nullptr,
                                                 &p->pipeline);
    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);
    if (r != VK_SUCCESS) {
        delete p;
        fatal("graphics pipeline %s failed to build (VkResult %d, %s)", name, (int)r,
              vk_result_name(r));
    }
    return p;
}

void VulkanDevice::destroy_graphics_pipeline(GraphicsPipeline* p) {
    if (!p) return;
    if (p->pipeline) vkDestroyPipeline(m_device, p->pipeline, nullptr);
    delete p;
}

/* ---- submission ----------------------------------------------------------- */

VkDescriptorSet VulkanDevice::allocate_descriptor_set() {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_frames[m_frame_index].descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkResult r = vkAllocateDescriptorSets(m_device, &ai, &set);
    if (r != VK_SUCCESS) {
        fatal("the frame's descriptor pool is exhausted (VkResult %d, %s); the renderer issued "
              "more dispatches and draws in one frame than the pool was sized for",
              (int)r, vk_result_name(r));
    }
    return set;
}

CommandList* VulkanDevice::begin_command_list() {
    if (m_recording) fatal("begin_command_list while a command list is still open");
    Frame& f = m_frames[m_frame_index];
    /* The frame's resources are reused, so everything the previous use of
     * this frame submitted has to have completed first. */
    wait(f.timeline);
    check(vkResetCommandPool(m_device, f.pool, 0), "vkResetCommandPool");
    check(vkResetDescriptorPool(m_device, f.descriptors, 0), "vkResetDescriptorPool");

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(f.cb, &bi), "vkBeginCommandBuffer");
    m_cmd->reset(f.cb);
    m_recording = true;
    return m_cmd;
}

uint64_t VulkanDevice::submit(CommandList* cmd) {
    if (!m_recording || cmd != m_cmd) fatal("submit of a command list that is not open");
    Frame& f = m_frames[m_frame_index];
    check(vkEndCommandBuffer(f.cb), "vkEndCommandBuffer");
    m_recording = false;

    /* m_acquired rather than f.acquire: the acquire may have been taken on a
     * different frame index, because submit() is what advances that index. */
    const bool present_this_frame = m_backbuffer_index != UINT32_MAX
                                  && m_acquired != VK_NULL_HANDLE
                                  && m_cmd->touched_swapchain();

    VkCommandBufferSubmitInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cbi.commandBuffer = f.cb;

    VkSemaphoreSubmitInfo waits[1]{};
    uint32_t wait_count = 0;
    if (present_this_frame) {
        waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[0].semaphore = m_acquired;
        waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        wait_count = 1;
    }

    VkSemaphoreSubmitInfo signals[2]{};
    const uint64_t value = ++m_timeline_value;
    signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[0].semaphore = m_timeline;
    signals[0].value = value;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    uint32_t signal_count = 1;
    if (present_this_frame) {
        signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[1].semaphore = m_image_release[m_backbuffer_index];
        signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signal_count = 2;
    }

    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.waitSemaphoreInfoCount = wait_count;
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = signal_count;
    si.pSignalSemaphoreInfos = signals;
    check(vkQueueSubmit2(m_queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2");

    f.timeline = value;
    if (present_this_frame) {
        /* The acquire's signal has been taken by this submit, so the
         * semaphore is free for the next acquire once this submit completes,
         * which is what the frame timeline says. */
        m_acquired = VK_NULL_HANDLE;
        m_present_image = m_backbuffer_index;
    }
    /* The frame advances here rather than in present(), so a headless device
     * has two frames in flight exactly as a windowed one does. */
    m_frame_index = (m_frame_index + 1) % kFrames;
    return value;
}

void VulkanDevice::wait(uint64_t value) {
    if (value == 0) return;
    VkSemaphoreWaitInfo wi{};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_timeline;
    wi.pValues = &value;
    const VkResult r = vkWaitSemaphores(m_device, &wi, UINT64_MAX);
    if (r == VK_ERROR_DEVICE_LOST) {
        fatal("vkWaitSemaphores reports VkResult %d (%s): the Vulkan device was lost while "
              "waiting for timeline value %llu, and a lost device cannot be recovered from",
              (int)r, vk_result_name(r), (unsigned long long)value);
    }
    check(r, "vkWaitSemaphores");
}

void VulkanDevice::wait_idle() {
    const VkResult r = vkDeviceWaitIdle(m_device);
    if (r == VK_ERROR_DEVICE_LOST) {
        fatal("vkDeviceWaitIdle reports VkResult %d (%s): the Vulkan device was lost, and a "
              "lost device cannot be recovered from", (int)r, vk_result_name(r));
    }
    check(r, "vkDeviceWaitIdle");
}

/* ---- swapchain ------------------------------------------------------------ */

void VulkanDevice::create_swapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu, m_surface, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = width ? width : 640;
        extent.height = height ? height : 480;
    }
    if (extent.width == 0 || extent.height == 0) {
        /* A minimised window. Not an error: the swapchain is left absent and
         * acquire_backbuffer rebuilds it once the surface has an extent
         * again. Said once and at warn, because what it looks like from
         * outside is a window that shows nothing. */
        if (!m_said_no_extent) {
            m_said_no_extent = true;
            rt_log_warn("rhi", "the surface reports a %ux%u extent, so there is nothing to "
                               "build a swapchain on. Nothing is presented until the window "
                               "has a size; this is what a minimised window looks like.",
                        extent.width, extent.height);
        }
        m_surface_width = 0;
        m_surface_height = 0;
        m_swapchain_dirty = true;
        return;
    }

    /* The two transfer usages the present path needs. The blit writes the
     * backbuffer with vkCmdBlitImage (TRANSFER_DST) and the screenshot reads
     * it with vkCmdCopyImageToBuffer (TRANSFER_SRC). Both are optional in the
     * surface capabilities, so they are checked by name here rather than
     * discovered as a validation error at the first present. */
    const VkImageUsageFlags want_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((caps.supportedUsageFlags & want_usage) != want_usage) {
        fatal("this surface supports image usage 0x%x, and the present path needs colour "
              "attachment, transfer source and transfer destination (0x%x)",
              (unsigned)caps.supportedUsageFlags, (unsigned)want_usage);
    }

    uint32_t format_count = 0;
    VkResult fr = vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu, m_surface, &format_count, nullptr);
    if (fr != VK_SUCCESS && fr != VK_INCOMPLETE) {
        fatal("vkGetPhysicalDeviceSurfaceFormatsKHR (count) failed with VkResult %d (%s)",
              (int)fr, vk_result_name(fr));
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (format_count) {
        fr = vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu, m_surface, &format_count, formats.data());
        if (fr != VK_SUCCESS && fr != VK_INCOMPLETE) {
            fatal("vkGetPhysicalDeviceSurfaceFormatsKHR failed with VkResult %d (%s) for %u "
                  "formats", (int)fr, vk_result_name(fr), format_count);
        }
        shrink_to_written(formats, format_count, fr);
    }
    if (formats.empty()) fatal("the surface reports no supported format");
    VkSurfaceFormatKHR chosen{};
    chosen.format = VK_FORMAT_UNDEFINED;
    for (const VkSurfaceFormatKHR& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
            && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) {
        /* Not formats[0]. rhi::Format carries three formats and the overlay
         * pipeline is built for the one the backbuffer reports, so a surface
         * format outside that set becomes Format::Unknown, then
         * VK_FORMAT_UNDEFINED in the pipeline, and vkCreateGraphicsPipelines
         * fails with a message about the pipeline rather than about the
         * surface. The formats the surface did offer are named instead. */
        std::string offered;
        for (const VkSurfaceFormatKHR& f : formats) {
            char one[64];
            std::snprintf(one, sizeof(one), "%s%d/%d", offered.empty() ? "" : ", ",
                          (int)f.format, (int)f.colorSpace);
            offered += one;
        }
        fatal("this surface offers no B8G8R8A8_UNORM or R8G8B8A8_UNORM in the sRGB "
              "non-linear colour space, and the renderer presents into one of those two. "
              "It offers (VkFormat/VkColorSpaceKHR): %s", offered.c_str());
    }

    uint32_t mode_count = 0;
    VkResult mr = vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu, m_surface, &mode_count,
                                                            nullptr);
    if (mr != VK_SUCCESS && mr != VK_INCOMPLETE) {
        /* Not a stop: FIFO is guaranteed, and the fallback below picks it.
         * But it is a query that did not answer, so the mode the run ends up
         * with is not the one the setting asked for and that has to be said. */
        rt_log_warn("rhi", "vkGetPhysicalDeviceSurfacePresentModesKHR (count) returned VkResult "
                           "%d (%s); the present mode this surface supports is unknown and the "
                           "fallback below applies", (int)mr, vk_result_name(mr));
    }
    std::vector<VkPresentModeKHR> modes(mode_count);
    if (mode_count) {
        mr = vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu, m_surface, &mode_count,
                                                       modes.data());
        if (mr != VK_SUCCESS && mr != VK_INCOMPLETE) {
            rt_log_warn("rhi", "vkGetPhysicalDeviceSurfacePresentModesKHR returned VkResult %d "
                               "(%s) for %u modes; the fallback below applies",
                        (int)mr, vk_result_name(mr), mode_count);
        }
        shrink_to_written(modes, mode_count, mr);
    }
    VkPresentModeKHR want = to_vk_present(m_present_mode);
    bool have_want = false;
    for (VkPresentModeKHR m : modes) {
        if (m == want) { have_want = true; break; }
    }
    if (!have_want) {
        /* FIFO is the one mode Vulkan guarantees, so it is the only honest
         * fallback, and the substitution is named rather than silent. */
        rt_log_warn("rhi", "present mode %u (VkPresentModeKHR %d) is not among the %u this "
                           "surface supports; using fifo, which Vulkan guarantees",
                    (unsigned)m_present_mode, (int)want, mode_count);
        want = VK_PRESENT_MODE_FIFO_KHR;
    }

    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && images > caps.maxImageCount) images = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = m_surface;
    ci.minImageCount = images;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = want;
    ci.clipped = VK_TRUE;
    /* The retired handle from a resize, or null on the first create. Passing
     * it lets the driver reuse the presentable images rather than tear the
     * surface down and build it again, and it is what the spec expects of a
     * recreate on a live surface. */
    ci.oldSwapchain = m_retired_swapchain;
    check(vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain), "vkCreateSwapchainKHR");

    uint32_t count = 0;
    VkResult ir = vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
    if (ir != VK_SUCCESS && ir != VK_INCOMPLETE) {
        fatal("vkGetSwapchainImagesKHR (count) failed with VkResult %d (%s), so the swapchain "
              "that was just created has no images to draw into", (int)ir, vk_result_name(ir));
    }
    std::vector<VkImage> vk_images(count);
    if (count) {
        ir = vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, vk_images.data());
        if (ir != VK_SUCCESS && ir != VK_INCOMPLETE) {
            fatal("vkGetSwapchainImagesKHR failed with VkResult %d (%s) for %u images",
                  (int)ir, vk_result_name(ir), count);
        }
        shrink_to_written(vk_images, count, ir);
    }
    if (vk_images.empty()) {
        fatal("the swapchain reports no images, so nothing can be drawn into it");
    }
    for (VkImage image : vk_images) {
        Texture* t = new Texture();
        t->image = image;
        t->format = chosen.format;
        t->width = extent.width;
        t->height = extent.height;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->owns_image = false;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = chosen.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        check(vkCreateImageView(m_device, &vi, nullptr, &t->view), "swapchain image view");
        m_backbuffers.push_back(t);

        /* One release semaphore per image. The submit that draws into image i
         * signals this one and vkQueuePresentKHR waits on it, so it can only
         * be signalled again once image i has come back out of an acquire,
         * which is the one thing that says the presentation engine is done
         * with the previous wait. */
        VkSemaphore release = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        check(vkCreateSemaphore(m_device, &si, nullptr, &release), "image release semaphore");
        m_image_release.push_back(release);
    }
    m_swapchain_format = chosen.format;
    m_surface_width = extent.width;
    m_surface_height = extent.height;
    m_swapchain_dirty = false;
    /* Both latches belong to the generation that just ended, not to the run:
     * a second minimise, or a second surface that goes suboptimal, is a new
     * fact and is said again. */
    m_said_no_extent = false;
    m_said_suboptimal_present = false;
    rt_log_info("rhi", "swapchain: %ux%u, VkFormat %d, VkPresentModeKHR %d, %u image(s)",
                extent.width, extent.height, (int)chosen.format, (int)want,
                (unsigned)m_backbuffers.size());
}

/* Everything the generation before last still held. Called from the two
 * places a full swapchain lifetime has demonstrably passed: the next
 * recreate, and the destructor after its vkDeviceWaitIdle. */
void VulkanDevice::free_retired_swapchain() {
    for (VkSemaphore s : m_retired_release) {
        if (s) vkDestroySemaphore(m_device, s, nullptr);
    }
    m_retired_release.clear();
    if (m_retired_swapchain) {
        vkDestroySwapchainKHR(m_device, m_retired_swapchain, nullptr);
        m_retired_swapchain = VK_NULL_HANDLE;
    }
}

/* With retire set, the swapchain handle and its per-image release semaphores
 * are not destroyed: the handle goes to vkCreateSwapchainKHR as oldSwapchain
 * and both are freed one generation later. vkDeviceWaitIdle, which the
 * caller has already done, covers the queue but says nothing about the
 * presentation engine having finished waiting on a semaphore that went to
 * vkQueuePresentKHR, which is what VUID-vkDestroySemaphore-semaphore-01137
 * is about. Keeping them one generation is what makes that wait moot. */
void VulkanDevice::destroy_swapchain(bool retire) {
    for (Texture* t : m_backbuffers) destroy_texture(t);
    m_backbuffers.clear();
    free_retired_swapchain();
    if (retire) {
        m_retired_release.swap(m_image_release);
        m_retired_swapchain = m_swapchain;
        m_swapchain = VK_NULL_HANDLE;
    }
    for (VkSemaphore s : m_image_release) {
        if (s) vkDestroySemaphore(m_device, s, nullptr);
    }
    m_image_release.clear();
    if (m_swapchain) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_backbuffer_index = UINT32_MAX;
    m_present_image = UINT32_MAX;
    /* acquire_backbuffer drains any outstanding acquire before it gets here,
     * and the destructor waits the device idle and then destroys the frames'
     * semaphores, so there is nothing left for this handle to name. */
    m_acquired = VK_NULL_HANDLE;
}

void VulkanDevice::set_present_mode(PresentMode mode) {
    if (mode == m_present_mode) return;
    m_present_mode = mode;
    m_swapchain_dirty = true;
}

void VulkanDevice::notify_resize(uint32_t width, uint32_t height) {
    m_surface_width = width;
    m_surface_height = height;
    m_swapchain_dirty = true;
}

/* Takes the signal off an acquire semaphore no submit ever waited on.
 *
 * vkAcquireNextImageKHR requires a semaphore with no signal and no wait
 * pending on it (VUID-vkAcquireNextImageKHR-semaphore-01779), so a frame that
 * acquired an image and then submitted nothing that touched it would leave
 * the semaphore signalled and the next acquire on that semaphore invalid. An
 * empty submit that waits on it and signals the timeline takes the signal,
 * and waiting the timeline value makes that observable to the host. */
void VulkanDevice::drain_acquire() {
    if (m_acquired == VK_NULL_HANDLE) return;
    if (!m_said_unused_acquire) {
        m_said_unused_acquire = true;
        /* warn, not info: the field that acquired this image reached no
         * screen, which is a frame the user did not get. */
        rt_log_warn("rhi", "a swapchain image was acquired and no submit drew into it; "
                           "its acquire semaphore is drained before reuse. Said once.");
    }
    VkSemaphoreSubmitInfo wait_info{};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_info.semaphore = m_acquired;
    wait_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    const uint64_t value = ++m_timeline_value;
    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = m_timeline;
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait_info;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    check(vkQueueSubmit2(m_queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2 (acquire drain)");
    m_acquired = VK_NULL_HANDLE;
    wait(value);
}

bool VulkanDevice::acquire_backbuffer(Texture** out) {
    if (out) *out = nullptr;
    m_backbuffer_index = UINT32_MAX;
    if (m_surface == VK_NULL_HANDLE) {
        /* A headless device, or one whose surface was never created. The
         * caller gets false and, without this, no reason for it. */
        note_present_stall("this device has no surface, so there is no swapchain to acquire "
                           "an image from");
        return false;
    }

    /* Before anything else, so no acquire semaphore is ever handed to a
     * second acquire with the first one's signal still standing. */
    drain_acquire();

    if (m_swapchain_dirty || m_swapchain == VK_NULL_HANDLE) {
        const VkResult idle = vkDeviceWaitIdle(m_device);
        if (idle == VK_ERROR_DEVICE_LOST) {
            fatal("vkDeviceWaitIdle before a swapchain rebuild reports VkResult %d (%s): the "
                  "Vulkan device was lost, and a lost device cannot be recovered from",
                  (int)idle, vk_result_name(idle));
        }
        if (idle != VK_SUCCESS) {
            rt_log_error("rhi", "vkDeviceWaitIdle before a swapchain rebuild failed with "
                                "VkResult %d (%s); the old swapchain is torn down with work "
                                "possibly still in flight", (int)idle, vk_result_name(idle));
        }
        destroy_swapchain(/*retire=*/true);
        create_swapchain(m_surface_width, m_surface_height);
        if (m_swapchain == VK_NULL_HANDLE) {
            if (m_surface_width == 0 || m_surface_height == 0) {
                note_present_stall("the surface has a %ux%u extent, which is what a minimised "
                                   "window looks like. This is expected, and presentation "
                                   "starts again when the window has a size",
                                   m_surface_width, m_surface_height);
            } else {
                note_present_stall("a swapchain rebuild at %ux%u produced none",
                                   m_surface_width, m_surface_height);
            }
            return false;
        }
    }

    Frame& f = m_frames[m_frame_index];
    /* This frame's previous submit waited on this same semaphore, and a
     * semaphore with a wait still pending cannot be given to an acquire
     * either. The frame's timeline value is what says that submit is done.
     * begin_command_list waits the same value for the frame's pool and
     * descriptors; this wait is here because the acquire happens first. */
    wait(f.timeline);

    uint32_t index = 0;
    const VkResult r = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                             f.acquire, VK_NULL_HANDLE, &index);
    if (r == VK_SUBOPTIMAL_KHR) {
        /* The image was acquired and the semaphore signalled, so the signal
         * has to be taken even though this frame is abandoned. */
        m_acquired = f.acquire;
        m_backbuffer_index = index;
        drain_acquire();
        m_backbuffer_index = UINT32_MAX;
        m_swapchain_dirty = true;
        note_present_stall("vkAcquireNextImageKHR returned VkResult %d (%s): the swapchain no "
                           "longer matches the surface, so this field is dropped and the "
                           "swapchain is rebuilt at the next acquire",
                           (int)r, vk_result_name(r));
        return false;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        /* Nothing was acquired and nothing was signalled. */
        m_swapchain_dirty = true;
        note_present_stall("vkAcquireNextImageKHR returned VkResult %d (%s): no image was "
                           "acquired and the swapchain is rebuilt at the next acquire",
                           (int)r, vk_result_name(r));
        return false;
    }
    if (r == VK_ERROR_DEVICE_LOST) {
        fatal("vkAcquireNextImageKHR reports VkResult %d (%s): the Vulkan device was lost "
              "during acquire, and a lost device cannot be recovered from",
              (int)r, vk_result_name(r));
    }
    if (r != VK_SUCCESS) {
        check(r, "vkAcquireNextImageKHR");
    }
    m_acquired = f.acquire;
    m_backbuffer_index = index;
    if (out) *out = m_backbuffers[index];
    return true;
}

void VulkanDevice::present() {
    if (m_swapchain == VK_NULL_HANDLE) {
        note_present_stall("present was called with no swapchain; the next acquire rebuilds "
                           "one, and nothing reaches the screen until it does");
        return;
    }
    if (m_present_image == UINT32_MAX) {
        if (m_backbuffer_index != UINT32_MAX && !m_said_present_without_draw) {
            m_said_present_without_draw = true;
            rt_log_warn("rhi", "present with an acquired image no submit drew into; "
                               "nothing is presented and the image is not handed back to "
                               "the swapchain, so a run that does this repeatedly will "
                               "block in acquire once every image is held");
        }
        note_present_stall(m_backbuffer_index != UINT32_MAX
                               ? "present had an acquired image that no submit drew into"
                               : "present was called with no image acquired");
        return;
    }
    /* vkQueuePresentKHR requires the image to be in PRESENT_SRC_KHR (the
     * VUID on VkPresentInfoKHR::pImageIndices). The caller's last barrier is
     * what puts it there, and a caller that forgets presents an image whose
     * contents the specification leaves undefined, which on some drivers is
     * the right picture and on others a black window. The layout this backend
     * tracked is checked rather than assumed, and a mismatch is named with
     * both layouts; the present still goes ahead, because a dropped present
     * would only turn one wrong frame into a frozen window. */
    if (m_present_image < m_backbuffers.size()
        && m_backbuffers[m_present_image]->layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_error("rhi", "swapchain image %u is in layout %d and vkQueuePresentKHR "
                                "requires VK_IMAGE_LAYOUT_PRESENT_SRC_KHR (%d); the caller's "
                                "last texture_barrier before present did not run or did not "
                                "target Stage::Present. Said once; what reaches the screen "
                                "from here on is undefined.",
                         (unsigned)m_present_image,
                         (int)m_backbuffers[m_present_image]->layout,
                         (int)VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
    }
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &m_image_release[m_present_image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_swapchain;
    pi.pImageIndices = &m_present_image;
    const VkResult r = vkQueuePresentKHR(m_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        /* The image was not presented. */
        m_swapchain_dirty = true;
        note_present_stall("vkQueuePresentKHR returned VkResult %d (%s): the image was not "
                           "presented and the swapchain is rebuilt at the next acquire",
                           (int)r, vk_result_name(r));
    } else if (r == VK_SUBOPTIMAL_KHR) {
        /* This one did reach the screen, just not at the surface's preferred
         * properties, so it is not counted as a stalled field. Once per
         * swapchain generation: create_swapchain re-arms the latch. */
        m_swapchain_dirty = true;
        if (!m_said_suboptimal_present) {
            m_said_suboptimal_present = true;
            rt_log_warn("rhi", "vkQueuePresentKHR returned VkResult %d (%s): the image was "
                               "presented but no longer matches the surface, so the swapchain "
                               "is rebuilt at the next acquire. Said once per swapchain.",
                        (int)r, vk_result_name(r));
        }
        note_presentation_resumed();
    } else if (r == VK_ERROR_DEVICE_LOST) {
        fatal("vkQueuePresentKHR reports VkResult %d (%s): the Vulkan device was lost during "
              "present, and a lost device cannot be recovered from", (int)r, vk_result_name(r));
    } else if (r != VK_SUCCESS) {
        check(r, "vkQueuePresentKHR");
    } else {
        note_presentation_resumed();
    }
    m_backbuffer_index = UINT32_MAX;
    m_present_image = UINT32_MAX;
}

/* ---- readback ------------------------------------------------------------- */

bool VulkanDevice::read_texture(Texture* t, std::vector<uint8_t>& out,
                                uint32_t* width, uint32_t* height) {
    if (!t || t->width == 0 || t->height == 0) {
        /* The caller (a screenshot, a parity dump) gets nothing back, and the
         * false alone says neither which of the three it was nor that it
         * happened at all. */
        rt_log_error("rhi", "read_texture of %s; nothing is read back",
                     !t ? "a null texture"
                        : (t->width == 0 ? "a texture with width 0" : "a texture with height 0"));
        return false;
    }
    if (t->format != VK_FORMAT_R8G8B8A8_UNORM && t->format != VK_FORMAT_B8G8R8A8_UNORM) {
        fatal("read_texture of a format this path does not pack (VkFormat %d)", (int)t->format);
    }
    if (!t->owns_image) {
        /* This path leaves the image in TRANSFER_SRC_OPTIMAL, because the
         * caller alone knows what the image is for next. On a swapchain image
         * that is the layout vkQueuePresentKHR forbids, so the next present
         * would be undefined. A caller that wants the presented picture
         * records copy_texture_to_buffer inside the present command list,
         * which is what capture_shot does, and keeps the final barrier to
         * Stage::Present. */
        fatal("read_texture of a swapchain image; it would leave the image in "
              "VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL and vkQueuePresentKHR requires "
              "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR");
    }
    const uint64_t bytes = (uint64_t)t->width * t->height * 4;

    BufferDesc bd;
    bd.size = bytes;
    bd.kind = BufferKind::Readback;
    bd.usage = BufferUsage::CopyDst;
    bd.debug_name = "rhi readback";
    Buffer* staging = create_buffer(bd);

    CommandList* cmd = begin_command_list();
    cmd->copy_texture_to_buffer(staging, 0, t);
    wait(submit(cmd));

    out.resize((size_t)bytes);
    std::memcpy(out.data(), staging->mapped, (size_t)bytes);
    /* B8G8R8A8 backbuffers come back with the channels swapped relative to
     * the caller's RGBA contract, so they are put right here rather than
     * every caller having to ask what format the surface picked. */
    if (t->format == VK_FORMAT_B8G8R8A8_UNORM) {
        for (size_t i = 0; i + 3 < out.size(); i += 4) {
            const uint8_t b = out[i];
            out[i] = out[i + 2];
            out[i + 2] = b;
        }
    }
    if (width) *width = t->width;
    if (height) *height = t->height;
    destroy_buffer(staging);
    return true;
}

Device* create_vulkan_device(const DeviceDesc& desc) {
    return new VulkanDevice(desc);
}

} // namespace rhi
