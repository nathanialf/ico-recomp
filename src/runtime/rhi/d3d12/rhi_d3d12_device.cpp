/* rhi/d3d12/rhi_d3d12_device.cpp: device, resources, pipelines, swapchain.
 *
 * Ours (MIT). See rhi_d3d12.h for the shape of the backend, rhi.h for the
 * interface it implements and rhi_d3d12_bindings.h for the register
 * convention the root signature and the generated HLSL both obey.
 */
#include "rhi_d3d12.h"
#include "rhi_d3d12_shaders.h"

#include "../../runtime.h"

#include <cstdarg>
#include <cstdio>

/* Note on ID3D12InfoQueue::GetMessage below: windows.h defines GetMessage as
 * GetMessageA (or GetMessageW under UNICODE), and d3d12sdklayers.h is parsed
 * with that macro in force, so the member is actually declared as GetMessageA.
 * The call is written unadorned and left for the macro to rewrite, which is
 * what makes the declaration and the call agree under either setting.
 * Undefining the macro here would break the call, not fix it. */

namespace rhi {

namespace {

/* Feature level 12_0 is the floor; rhi_d3d12.h says why (resource binding
 * tier 2, for the twenty UAVs in one table). */
const D3D_FEATURE_LEVEL kFeatureLevel = D3D_FEATURE_LEVEL_12_0;

const char* heap_name(BufferKind k) {
    switch (k) {
        case BufferKind::Upload:   return "upload";
        case BufferKind::Readback: return "readback";
        default:                   return "device local";
    }
}

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

} // namespace

DXGI_FORMAT to_dxgi_format(Format f) {
    switch (f) {
        case Format::RGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::R32Uint:    return DXGI_FORMAT_R32_UINT;
        default: break;
    }
    /* UNKNOWN is not a format any resource or pipeline here can be built
     * with, so the caller is about to fail with E_INVALIDARG and nothing
     * saying which value caused it. Named here, once per distinct value,
     * while the value is still in hand. */
    static bool said_any = false;
    static Format said = Format::Unknown;
    if (!said_any || f != said) {
        said_any = true;
        said = f;
        rt_log_error("rhi", "rhi Format %u has no DXGI format in this backend; the "
                            "resource or pipeline asking for it will fail to build",
                     (unsigned)f);
    }
    return DXGI_FORMAT_UNKNOWN;
}

/* ---- HRESULT naming --------------------------------------------------------
 *
 * Every failure line below carries the raw code and this name, because a bare
 * 0x887a0005 in a user's log is a search engine round trip before anyone knows
 * what happened. The list holds the codes the calls this backend makes can
 * actually return; anything else prints as unrecognized rather than being
 * given a name nobody looked up.
 */
const char* d3d12_hresult_name(HRESULT hr) {
    switch (hr) {
        case S_OK:                             return "S_OK";
        case S_FALSE:                          return "S_FALSE";
        case E_FAIL:                           return "E_FAIL";
        case E_INVALIDARG:                     return "E_INVALIDARG";
        case E_OUTOFMEMORY:                    return "E_OUTOFMEMORY";
        case E_NOINTERFACE:                    return "E_NOINTERFACE";
        case E_NOTIMPL:                        return "E_NOTIMPL";
        case DXGI_STATUS_OCCLUDED:             return "DXGI_STATUS_OCCLUDED";
        case DXGI_ERROR_INVALID_CALL:          return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_NOT_FOUND:             return "DXGI_ERROR_NOT_FOUND";
        case DXGI_ERROR_UNSUPPORTED:           return "DXGI_ERROR_UNSUPPORTED";
        case DXGI_ERROR_DEVICE_REMOVED:        return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_HUNG:           return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_RESET:          return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_WAS_STILL_DRAWING:     return "DXGI_ERROR_WAS_STILL_DRAWING";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_SDK_COMPONENT_MISSING: return "DXGI_ERROR_SDK_COMPONENT_MISSING";
        case DXGI_ERROR_ACCESS_LOST:           return "DXGI_ERROR_ACCESS_LOST";
        default:                               return "unrecognized HRESULT";
    }
}

/* The three codes a call returns when the device itself is gone.
 * DXGI_ERROR_DRIVER_INTERNAL_ERROR is deliberately not here: it is named
 * above because GetDeviceRemovedReason can report it, but it is not one of
 * the three a call returns to say the device has been lost. */
bool d3d12_device_lost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
        || hr == DXGI_ERROR_DEVICE_HUNG;
}

void D3D12Device::fatal(const char* fmt, ...) const {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    /* The adapter name is in every fatal for the reason the Vulkan backend
     * gives: the same message from two machines is the same bug only if it is
     * the same driver. */
    rt_fatal("rhi", nullptr, "%s (adapter: %s, %s)", msg, m_device_name.c_str(),
             m_api_version.empty() ? "no feature level yet" : m_api_version.c_str());
}

void D3D12Device::check(HRESULT hr, const char* what) const {
    if (SUCCEEDED(hr)) return;
    /* A lost device gets its own path: the code the call returned says only
     * that it is gone, and GetDeviceRemovedReason is the one that says why. */
    if (d3d12_device_lost(hr)) fatal_device_removed(what, hr);
    fatal("%s failed with HRESULT 0x%08lx (%s)", what, (unsigned long)hr,
          d3d12_hresult_name(hr));
}

void D3D12Device::fatal_device_removed(const char* what, HRESULT hr) const {
    /* Asked of the device rather than inferred from `hr`: the call's own code
     * is the same for every cause, and the removed reason distinguishes a
     * driver timeout (HUNG) from a driver reset, an unplugged adapter and an
     * internal driver error. */
    const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : hr;
    fatal("%s reported the Direct3D 12 device gone: HRESULT 0x%08lx (%s), "
          "GetDeviceRemovedReason 0x%08lx (%s). Nothing recovers from a removed device, "
          "so the run ends here rather than continuing on a dead one.",
          what, (unsigned long)hr, d3d12_hresult_name(hr),
          (unsigned long)reason, d3d12_hresult_name(reason));
}

void D3D12Device::note_present_skipped(const char* why) {
    ++m_skipped_fields;
    m_present_stalled = true;
    if (m_said_present_stalled) return;
    m_said_present_stalled = true;
    rt_log_warn("rhi", "nothing is being presented: %s. Further skipped fields are "
                       "counted rather than logged, and one line says so when "
                       "presentation resumes.", why);
}

void D3D12Device::note_present_resumed() {
    if (!m_present_stalled) return;
    m_present_stalled = false;
    /* Both latches are re-armed, so a later stall is reported as loudly as
     * this one was rather than passing in silence. */
    m_said_present_stalled = false;
    m_said_no_client_area = false;
    rt_log_info("rhi", "presentation resumed after %llu skipped field%s",
                (unsigned long long)m_skipped_fields, m_skipped_fields == 1 ? "" : "s");
    m_skipped_fields = 0;
}

void D3D12Device::drain_debug_messages() {
    if (!m_info_queue) return;
    const UINT64 count = m_info_queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T bytes = 0;
        /* A dropped debug-layer message is worth a line of its own: this is
         * the channel a pipeline failure is diagnosed through, and losing one
         * silently is how a diagnostic run comes back empty. Said once,
         * because a queue that fails once usually fails for every message. */
        HRESULT mhr = m_info_queue->GetMessage(i, nullptr, &bytes);
        if (FAILED(mhr) || bytes == 0) {
            static bool said_size = false;
            if (!said_size) {
                said_size = true;
                rt_log_warn("rhi", "ID3D12InfoQueue::GetMessage could not size message %llu "
                                   "(HRESULT 0x%08lx, %s, %zu bytes); that debug layer "
                                   "message is lost",
                            (unsigned long long)i, (unsigned long)mhr,
                            d3d12_hresult_name(mhr), (size_t)bytes);
            }
            continue;
        }
        std::vector<uint8_t> storage(bytes);
        D3D12_MESSAGE* m = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        mhr = m_info_queue->GetMessage(i, m, &bytes);
        if (FAILED(mhr)) {
            static bool said_read = false;
            if (!said_read) {
                said_read = true;
                rt_log_warn("rhi", "ID3D12InfoQueue::GetMessage could not read message %llu "
                                   "(HRESULT 0x%08lx, %s); that debug layer message is lost",
                            (unsigned long long)i, (unsigned long)mhr,
                            d3d12_hresult_name(mhr));
            }
            continue;
        }
        if (m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION
            || m->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
            rt_log_error("rhi", "debug layer: %s", m->pDescription);
        } else if (m->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
            rt_log_warn("rhi", "debug layer: %s", m->pDescription);
        } else {
            rt_log_debug("rhi", "debug layer: %s", m->pDescription);
        }
    }
    m_info_queue->ClearStoredMessages();
}

/* ---- pipeline failure diagnostics ------------------------------------------
 *
 * CreateGraphicsPipelineState and CreateComputePipelineState return
 * E_INVALIDARG and nothing else. The debug layer says which rule was broken,
 * but it is an optional Windows feature and most players do not have it, so
 * the description itself is printed here from the struct that was handed in.
 * Everything below is host state: no guest memory is read.
 */

const char* dxgi_format_name(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_UNKNOWN:            return "UNKNOWN";
        case DXGI_FORMAT_R8G8B8A8_UNORM:     return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM:     return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R32G32_FLOAT:       return "R32G32_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R32_UINT:           return "R32_UINT";
        case DXGI_FORMAT_R32_TYPELESS:       return "R32_TYPELESS";
        default:                             return "(other)";
    }
}

void log_shader_stage(const char* stage, const ShaderBytecode& sh) {
    rt_log_error("rhi", "  %s: %s, %zu bytes, from %s", stage, sh.name, sh.bytes.size(),
                 sh.from_dxil ? "the committed DXIL (rhi_shaders_dxil.h)"
                              : "dxcompiler.dll at run time");
}

/* The root signature this backend builds, described from the same constants
 * create_root_signature uses, so the two cannot drift. */
void log_root_signature() {
    using namespace d3d12_bind;
    rt_log_error("rhi", "  root signature: %u parameters, 0 static samplers, "
                        "ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT",
                 (unsigned)kRootParamCount);
    rt_log_error("rhi", "    [%u] 32-bit constants, b%u space%u, %u DWORDs, visibility ALL",
                 (unsigned)kRootParamConstants, (unsigned)kRootConstantRegister,
                 (unsigned)kRootConstantSpace, (unsigned)kRootConstantDwords);
    rt_log_error("rhi", "    [%u] table: CBV b%u..b%u, UAV u%u..u%u, SRV t%u..t%u, "
                        "UAV u%u..u%u, all space%u, visibility ALL",
                 (unsigned)kRootParamTable,
                 (unsigned)kUniformBufferBaseRegister,
                 (unsigned)(kUniformBufferBaseRegister + kMaxUniformBuffers - 1),
                 (unsigned)kStorageBufferBaseRegister,
                 (unsigned)(kStorageBufferBaseRegister + kMaxStorageBuffers - 1),
                 (unsigned)kSampledTextureBaseRegister,
                 (unsigned)(kSampledTextureBaseRegister + kMaxSampledTextures - 1),
                 (unsigned)kStorageImageBaseRegister,
                 (unsigned)(kStorageImageBaseRegister + kMaxStorageImages - 1),
                 (unsigned)kRegisterSpace);
    rt_log_error("rhi", "    [%u] table: sampler s%u..s%u space%u, visibility ALL",
                 (unsigned)kRootParamSamplers, (unsigned)kSamplerBaseRegister,
                 (unsigned)(kSamplerBaseRegister + kSamplerCount - 1),
                 (unsigned)kRegisterSpace);
}

void D3D12Device::report_graphics_pipeline_failure(
    const char* name, HRESULT hr, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd,
    const ShaderBytecode& vs, const ShaderBytecode& ps) {
    rt_log_error("rhi", "CreateGraphicsPipelineState failed for %s (HRESULT 0x%08lx, %s). "
                        "The description it was given:", name, (unsigned long)hr,
                 d3d12_hresult_name(hr));
    log_shader_stage("VS", vs);
    log_shader_stage("PS", ps);
    rt_log_error("rhi", "  input layout: %u elements", (unsigned)pd.InputLayout.NumElements);
    for (UINT i = 0; i < pd.InputLayout.NumElements; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& e = pd.InputLayout.pInputElementDescs[i];
        rt_log_error("rhi", "    [%u] %s%u %s slot %u offset %u %s step %u", (unsigned)i,
                     e.SemanticName, (unsigned)e.SemanticIndex,
                     dxgi_format_name(e.Format), (unsigned)e.InputSlot,
                     (unsigned)e.AlignedByteOffset,
                     e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
                         ? "per-vertex" : "per-instance",
                     (unsigned)e.InstanceDataStepRate);
    }
    rt_log_error("rhi", "  %u render targets, RTV[0] %s, DSV %s, samples %u quality %u, "
                        "topology type %u",
                 (unsigned)pd.NumRenderTargets, dxgi_format_name(pd.RTVFormats[0]),
                 dxgi_format_name(pd.DSVFormat), (unsigned)pd.SampleDesc.Count,
                 (unsigned)pd.SampleDesc.Quality, (unsigned)pd.PrimitiveTopologyType);
    const D3D12_RENDER_TARGET_BLEND_DESC& rt = pd.BlendState.RenderTarget[0];
    rt_log_error("rhi", "  blend[0]: enable %d logic %d src %u dst %u op %u, "
                        "srcA %u dstA %u opA %u, logicop %u, mask 0x%x; "
                        "independent %d alpha-to-coverage %d, sample mask 0x%08x",
                 (int)rt.BlendEnable, (int)rt.LogicOpEnable, (unsigned)rt.SrcBlend,
                 (unsigned)rt.DestBlend, (unsigned)rt.BlendOp, (unsigned)rt.SrcBlendAlpha,
                 (unsigned)rt.DestBlendAlpha, (unsigned)rt.BlendOpAlpha,
                 (unsigned)rt.LogicOp, (unsigned)rt.RenderTargetWriteMask,
                 (int)pd.BlendState.IndependentBlendEnable,
                 (int)pd.BlendState.AlphaToCoverageEnable, (unsigned)pd.SampleMask);
    rt_log_error("rhi", "  raster: fill %u cull %u ccw %d depth-clip %d; depth enable %d "
                        "stencil enable %d",
                 (unsigned)pd.RasterizerState.FillMode,
                 (unsigned)pd.RasterizerState.CullMode,
                 (int)pd.RasterizerState.FrontCounterClockwise,
                 (int)pd.RasterizerState.DepthClipEnable,
                 (int)pd.DepthStencilState.DepthEnable,
                 (int)pd.DepthStencilState.StencilEnable);
    log_root_signature();
    drain_debug_messages();
    if (!m_validation) {
        rt_log_error("rhi", "  the D3D12 debug layer is off, so nothing above came from it. "
                            "Build with ICORECOMP_RHI_VALIDATION and run again to have it "
                            "name the broken rule.");
    }
    fatal("graphics pipeline %s failed to build (HRESULT 0x%08lx, %s)", name,
          (unsigned long)hr, d3d12_hresult_name(hr));
}

void D3D12Device::report_compute_pipeline_failure(
    const char* name, HRESULT hr, const D3D12_COMPUTE_PIPELINE_STATE_DESC& pd,
    const ShaderBytecode& cs) {
    rt_log_error("rhi", "CreateComputePipelineState failed for %s (HRESULT 0x%08lx, %s). "
                        "The description it was given:", name, (unsigned long)hr,
                 d3d12_hresult_name(hr));
    log_shader_stage("CS", cs);
    rt_log_error("rhi", "  CS bytecode in the description: %zu bytes, node mask %u, "
                        "flags %u", (size_t)pd.CS.BytecodeLength, (unsigned)pd.NodeMask,
                 (unsigned)pd.Flags);
    log_root_signature();
    drain_debug_messages();
    if (!m_validation) {
        rt_log_error("rhi", "  the D3D12 debug layer is off, so nothing above came from it. "
                            "Build with ICORECOMP_RHI_VALIDATION and run again to have it "
                            "name the broken rule.");
    }
    fatal("compute pipeline %s failed to build (HRESULT 0x%08lx, %s)", name,
          (unsigned long)hr, d3d12_hresult_name(hr));
}

/* ---- construction --------------------------------------------------------- */

D3D12Device::D3D12Device(const DeviceDesc& desc) {
    m_validation = desc.validation;
    m_present_mode = desc.present_mode;
    m_software = desc.prefer_software;
    m_hwnd = desc.win32_hwnd;

    const char* why = nullptr;
    m_entries = d3d12_load_entries(&why);
    if (!m_entries) {
        rt_fatal("rhi", nullptr,
                 "Direct3D 12 is not available on this system (%s); the native GS "
                 "renderer cannot start on the d3d12 backend. Use ICORECOMP_GS_BACKEND=vulkan, "
                 "or run with the paraLLEl-GS backend (ICORECOMP_GS=parallel).",
                 why ? why : "d3d12.dll could not be loaded");
    }

    /* The debug layer is decided at build time (ICORECOMP_RHI_VALIDATION,
     * see gs/render/gs_native.cpp). Turning it on has to happen here, before
     * device creation, which is the only point D3D12 accepts it. */
    if (m_validation) {
        if (!m_entries->D3D12GetDebugInterface) {
            rt_log_warn("rhi", "the D3D12 debug layer was asked for (built with "
                               "ICORECOMP_RHI_VALIDATION) but this d3d12.dll exports no "
                               "D3D12GetDebugInterface; continuing without it");
            m_validation = false;
        } else {
            ComPtr<ID3D12Debug> debug;
            const HRESULT dhr = m_entries->D3D12GetDebugInterface(__uuidof(ID3D12Debug),
                                                                  debug.put_void());
            if (SUCCEEDED(dhr)) {
                debug->EnableDebugLayer();
                rt_log_info("rhi", "D3D12GetDebugInterface succeeded; the debug layer is on "
                                   "and its messages are drained into this log");
            } else {
                /* DXGI_ERROR_SDK_COMPONENT_MISSING (0x887A002D) is what a
                 * machine without the layer returns. The layer ships in the
                 * optional Windows feature called Graphics Tools, so the log
                 * names it rather than saying the interface was unavailable. */
                rt_log_warn("rhi", "D3D12GetDebugInterface failed with HRESULT 0x%08lx (%s), "
                                   "so the D3D12 debug layer is not installed on this machine. "
                                   "It is the optional Windows feature \"Graphics Tools\": "
                                   "Settings, System, Optional features, Add an optional "
                                   "feature, Graphics Tools. Continuing without it, so a "
                                   "pipeline that fails to build will print its own "
                                   "description and no debug-layer message.",
                            (unsigned long)dhr, d3d12_hresult_name(dhr));
                m_validation = false;
            }
        }
    }

    UINT factory_flags = 0;
    if (m_validation) factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    check(m_entries->CreateDXGIFactory2(factory_flags, __uuidof(IDXGIFactory2),
                                        m_factory.put_void()), "CreateDXGIFactory2");

    pick_adapter(desc);
    create_device(desc);
    create_root_signature();
    create_frames();
    m_cmd = new D3D12CommandList(this);
    create_dummies();
    if (!desc.headless) {
        if (!m_hwnd) {
            /* Loud, because the alternative is a run that draws into nothing
             * and looks like a hang. */
            rt_fatal("rhi", nullptr,
                     "a window was requested but no HWND was given to the D3D12 backend "
                     "(x11=%p wayland=%p); D3D12 presents to a Win32 window only",
                     desc.x11_display, desc.wl_display);
        }
        create_swapchain(desc.surface_width, desc.surface_height);
    }

    rt_log_info("rhi", "D3D12 device: %s (%s)%s%s%s", m_device_name.c_str(),
                m_api_version.c_str(),
                m_swapchain ? ", swapchain" : ", headless",
                m_validation ? ", debug layer on" : "",
                m_software ? ", software adapter (WARP)" : "");
}

D3D12Device::~D3D12Device() {
    if (!m_device) return;
    wait_idle();
    delete m_cmd;
    m_cmd = nullptr;
    destroy_swapchain();
    for (BlitPipeline& b : m_blit_pipelines) destroy_graphics_pipeline(b.pipeline);
    m_blit_pipelines.clear();
    if (m_dummy_buffer) destroy_buffer(m_dummy_buffer);
    if (m_dummy_uniform_buffer) destroy_buffer(m_dummy_uniform_buffer);
    if (m_dummy_texture) destroy_texture(m_dummy_texture);
    if (m_dummy_storage_image) destroy_texture(m_dummy_storage_image);
    if (m_fence_event) CloseHandle(m_fence_event);
}

void D3D12Device::pick_adapter(const DeviceDesc& desc) {
    ComPtr<IDXGIAdapter1> best;
    DXGI_ADAPTER_DESC1 best_desc{};

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        /* Any failure ends the walk, not only NOT_FOUND: on anything else
         * DXGI leaves the out pointer null, and the previous form went on to
         * dereference it. A short enumeration is also worth saying out loud,
         * because the fatal below would otherwise be the first news that the
         * machine's adapters were never looked at. */
        const HRESULT ehr = m_factory->EnumAdapters1(i, adapter.put());
        if (ehr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(ehr) || !adapter) {
            rt_log_error("rhi", "IDXGIFactory1::EnumAdapters1(%u) failed with HRESULT "
                                "0x%08lx (%s); the adapter walk stops here and only the %u "
                                "adapter(s) before it were considered",
                         (unsigned)i, (unsigned long)ehr, d3d12_hresult_name(ehr),
                         (unsigned)i);
            break;
        }
        DXGI_ADAPTER_DESC1 ad{};
        const HRESULT dhr = adapter->GetDesc1(&ad);
        if (FAILED(dhr)) {
            rt_log_warn("rhi", "IDXGIAdapter1::GetDesc1 failed for adapter %u with HRESULT "
                               "0x%08lx (%s); that adapter is skipped, and it cannot be "
                               "named because the name is what failed to read",
                        (unsigned)i, (unsigned long)dhr, d3d12_hresult_name(dhr));
            continue;
        }

        char name[256];
        std::snprintf(name, sizeof(name), "%ls", ad.Description);
        const bool software = (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        /* The software adapter is reachable only when it was asked for. CI
         * uses it on a runner with no GPU; a real machine must never fall
         * back to it silently, because a software rasteriser that runs at one
         * frame a second looks like a hang rather than a missing driver. */
        if (software != desc.prefer_software) {
            rt_log_debug("rhi", "skipping adapter %s: %s", name,
                         software ? "it is the software adapter and one was not asked for"
                                  : "a software adapter was asked for and this is hardware");
            continue;
        }

        /* A null out pointer makes D3D12CreateDevice a support probe. */
        const HRESULT probe = m_entries->D3D12CreateDevice(adapter.get(), kFeatureLevel,
                                                           __uuidof(ID3D12Device), nullptr);
        if (FAILED(probe)) {
            rt_log_warn("rhi", "skipping adapter %s: the feature level 12_0 probe failed "
                               "with HRESULT 0x%08lx (%s), and this renderer needs 12_0",
                        name, (unsigned long)probe, d3d12_hresult_name(probe));
            continue;
        }
        const bool better = !best || ad.DedicatedVideoMemory > best_desc.DedicatedVideoMemory;
        if (better) {
            best_desc = ad;
            best = std::move(adapter);
        }
    }

    if (!best && desc.prefer_software) {
        /* Some Windows versions do not enumerate WARP through EnumAdapters1.
         * IDXGIFactory4 names it directly. Each step says why it gave up:
         * this is the CI path, and the fatal below cannot tell a machine with
         * no WARP from one where the query for it failed. */
        ComPtr<IDXGIFactory4> factory4;
        const HRESULT qhr = m_factory->QueryInterface(__uuidof(IDXGIFactory4),
                                                      factory4.put_void());
        if (FAILED(qhr)) {
            rt_log_error("rhi", "a software adapter was asked for and none was enumerated, "
                                "and QueryInterface(IDXGIFactory4) failed with HRESULT "
                                "0x%08lx (%s), so WARP cannot be asked for by name",
                         (unsigned long)qhr, d3d12_hresult_name(qhr));
        } else {
            ComPtr<IDXGIAdapter1> warp;
            const HRESULT whr = factory4->EnumWarpAdapter(__uuidof(IDXGIAdapter1),
                                                          warp.put_void());
            if (FAILED(whr)) {
                rt_log_error("rhi", "IDXGIFactory4::EnumWarpAdapter failed with HRESULT "
                                    "0x%08lx (%s); this machine has no usable WARP adapter",
                             (unsigned long)whr, d3d12_hresult_name(whr));
            } else {
                const HRESULT probe = m_entries->D3D12CreateDevice(
                    warp.get(), kFeatureLevel, __uuidof(ID3D12Device), nullptr);
                if (FAILED(probe)) {
                    rt_log_error("rhi", "the WARP adapter does not support Direct3D feature "
                                        "level 12_0: the probe failed with HRESULT 0x%08lx "
                                        "(%s)", (unsigned long)probe,
                                 d3d12_hresult_name(probe));
                } else {
                    const HRESULT dhr = warp->GetDesc1(&best_desc);
                    if (FAILED(dhr)) {
                        rt_log_warn("rhi", "IDXGIAdapter1::GetDesc1 failed for the WARP "
                                           "adapter with HRESULT 0x%08lx (%s); it is used "
                                           "anyway and its name in the log below is whatever "
                                           "the zeroed description holds",
                                    (unsigned long)dhr, d3d12_hresult_name(dhr));
                    }
                    best = std::move(warp);
                }
            }
        }
    }

    if (!best) {
        rt_fatal("rhi", nullptr,
                 "no Direct3D 12 adapter on this system meets the renderer's requirements "
                 "(feature level 12_0%s)",
                 desc.prefer_software ? ", software adapter requested" : "");
    }
    char name[256];
    std::snprintf(name, sizeof(name), "%ls", best_desc.Description);
    m_device_name = name;
    m_software = (best_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    m_adapter = std::move(best);
}

void D3D12Device::create_device(const DeviceDesc& desc) {
    (void)desc;
    check(m_entries->D3D12CreateDevice(m_adapter.get(), kFeatureLevel,
                                       __uuidof(ID3D12Device), m_device.put_void()),
          "D3D12CreateDevice");

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const HRESULT fhr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                      sizeof(options));
    if (FAILED(fhr)) {
        fatal("CheckFeatureSupport(D3D12_OPTIONS) failed with HRESULT 0x%08lx (%s) on a "
              "device that reported feature level 12_0", (unsigned long)fhr,
              d3d12_hresult_name(fhr));
    }
    /* The one hard requirement beyond the feature level. Feature level 12_0
     * guarantees tier 2, so this can only fail on a driver that misreports,
     * and it is checked rather than assumed because the failure mode without
     * it is a table that silently binds nothing. */
    if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2) {
        fatal("the adapter reports resource binding tier %u; this renderer binds twenty "
              "UAVs in one descriptor table and needs tier 2",
              (unsigned)options.ResourceBindingTier);
    }
    /* Logged, not required. The storage images are written and never read, and
     * a typed UAV store to R8G8B8A8_UNORM is guaranteed at every feature level
     * this backend accepts; the additional-formats feature only matters for
     * typed loads, which no shader here does. */
    if (!options.TypedUAVLoadAdditionalFormats) {
        rt_log_debug("rhi", "the adapter does not support typed UAV loads of the "
                            "additional formats; nothing here needs them");
    }

    char v[128];
    std::snprintf(v, sizeof(v), "D3D12 feature level 12_0, resource binding tier %u",
                  (unsigned)options.ResourceBindingTier);
    m_api_version = v;

    /* What the renderer is allowed to ask this device for, filled in at
     * creation the way the Vulkan backend does. D3D12 offers no query: the
     * dispatch grid limit is a fixed 65535 per axis for every feature level
     * this backend accepts, and the header states it as
     * D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION. The constant is
     * preferred over the number so that a header that ever states something
     * else is what this reads. */
#ifdef D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION
    const uint32_t max_groups = (uint32_t)D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
#else
    /* This build's d3d12.h does not state it. 65535 is the documented value
     * for feature level 12_0 and above. */
    const uint32_t max_groups = 65535u;
#endif
    m_limits.max_workgroup_count[0] = max_groups;
    m_limits.max_workgroup_count[1] = max_groups;
    m_limits.max_workgroup_count[2] = max_groups;
    m_limits.frames_in_flight = kFrames;
    rt_log_debug("rhi", "device limits: compute workgroup grid %ux%ux%u, %u frames in flight",
                 m_limits.max_workgroup_count[0], m_limits.max_workgroup_count[1],
                 m_limits.max_workgroup_count[2], m_limits.frames_in_flight);

    if (m_validation) {
        /* Not check(): a device with no info queue still renders. But it
         * renders with the debug layer's messages going nowhere, which is the
         * one thing the layer was turned on for, so the run has to say so. */
        const HRESULT ihr = m_device->QueryInterface(__uuidof(ID3D12InfoQueue),
                                                     m_info_queue.put_void());
        if (FAILED(ihr) || !m_info_queue) {
            rt_log_warn("rhi", "the D3D12 debug layer is on but "
                               "QueryInterface(ID3D12InfoQueue) failed with HRESULT 0x%08lx "
                               "(%s); its messages cannot be drained into this log, so a "
                               "pipeline failure will print only its own description",
                        (unsigned long)ihr, d3d12_hresult_name(ihr));
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    check(m_device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), m_queue.put_void()),
          "CreateCommandQueue");

    check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                m_fence.put_void()), "CreateFence");
    m_fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!m_fence_event) {
        fatal("CreateEvent for the submission fence failed with Win32 error %lu; every "
              "wait on submitted work depends on it", (unsigned long)GetLastError());
    }

    m_descriptor_size =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_rtv_size = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kFrames * kSetsPerFrame * d3d12_bind::kDescriptorsPerSet;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(m_device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
                                         m_descriptor_heap.put_void()),
          "CreateDescriptorHeap (CBV/SRV/UAV)");

    /* rhi.h's four immutable samplers, in its order and with its filters:
     * 0 nearest/clamp, 1 linear/clamp, 2 nearest/repeat, 3 linear/repeat.
     * MIP_POINT with no LOD bias mirrors the Vulkan side's
     * VK_SAMPLER_MIPMAP_MODE_NEAREST and VK_LOD_CLAMP_NONE. Written once
     * here and never rewritten, which is what immutable means; the heap is
     * shader visible because a sampler descriptor table reads from one. */
    D3D12_DESCRIPTOR_HEAP_DESC sd{};
    sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sd.NumDescriptors = kSamplerCount;
    sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(m_device->CreateDescriptorHeap(&sd, __uuidof(ID3D12DescriptorHeap),
                                         m_sampler_heap.put_void()),
          "CreateDescriptorHeap (sampler)");

    const struct { D3D12_FILTER filter; D3D12_TEXTURE_ADDRESS_MODE address; }
    kSamplerSpecs[kSamplerCount] = {
        { D3D12_FILTER_MIN_MAG_MIP_POINT,        D3D12_TEXTURE_ADDRESS_MODE_CLAMP },
        { D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP },
        { D3D12_FILTER_MIN_MAG_MIP_POINT,        D3D12_TEXTURE_ADDRESS_MODE_WRAP  },
        { D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP  },
    };
    const UINT sampler_stride =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_handle =
        m_sampler_heap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
        D3D12_SAMPLER_DESC sam{};
        sam.Filter = kSamplerSpecs[i].filter;
        sam.AddressU = kSamplerSpecs[i].address;
        sam.AddressV = kSamplerSpecs[i].address;
        sam.AddressW = kSamplerSpecs[i].address;
        sam.MipLODBias = 0.0f;
        sam.MaxAnisotropy = 1;
        sam.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        /* Transparent black, which is Vulkan's default borderColor and what
         * rhi_vulkan_device.cpp leaves its samplers with. No address mode in
         * rhi.h's immutable set is BORDER, so the value is never reached; the
         * two backends describing the same sampler differently is the thing
         * worth removing. */
        sam.BorderColor[0] = 0.0f;
        sam.BorderColor[1] = 0.0f;
        sam.BorderColor[2] = 0.0f;
        sam.BorderColor[3] = 0.0f;
        sam.MinLOD = 0.0f;
        sam.MaxLOD = D3D12_FLOAT32_MAX;
        m_device->CreateSampler(&sam, sampler_handle);
        sampler_handle.ptr += sampler_stride;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rd.NumDescriptors = kRtvHeapSize;
    rd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    check(m_device->CreateDescriptorHeap(&rd, __uuidof(ID3D12DescriptorHeap),
                                         m_rtv_heap.put_void()),
          "CreateDescriptorHeap (RTV)");

    /* One dispatch argument, which is what dispatch_indirect passes. The
     * stride is the D3D12 argument layout: three UINTs, the same three words
     * a VkDispatchIndirectCommand holds, so the caller's buffer is laid out
     * identically for both backends. */
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC cs{};
    cs.ByteStride = 3 * sizeof(uint32_t);
    cs.NumArgumentDescs = 1;
    cs.pArgumentDescs = &arg;
    check(m_device->CreateCommandSignature(&cs, nullptr, __uuidof(ID3D12CommandSignature),
                                           m_dispatch_signature.put_void()),
          "CreateCommandSignature (dispatch)");
}

void D3D12Device::create_root_signature() {
    using namespace d3d12_bind;

    /* One table over the one shader-visible heap, laid out exactly as
     * rhi_d3d12_bindings.h documents. */
    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    ranges[0].NumDescriptors = kMaxUniformBuffers;
    ranges[0].BaseShaderRegister = kUniformBufferBaseRegister;
    ranges[0].RegisterSpace = kRegisterSpace;
    ranges[0].OffsetInDescriptorsFromTableStart = kTableUniformBuffers;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = kMaxStorageBuffers;
    ranges[1].BaseShaderRegister = kStorageBufferBaseRegister;
    ranges[1].RegisterSpace = kRegisterSpace;
    ranges[1].OffsetInDescriptorsFromTableStart = kTableStorageBuffers;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = kMaxSampledTextures;
    ranges[2].BaseShaderRegister = kSampledTextureBaseRegister;
    ranges[2].RegisterSpace = kRegisterSpace;
    ranges[2].OffsetInDescriptorsFromTableStart = kTableSampledTextures;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[3].NumDescriptors = kMaxStorageImages;
    ranges[3].BaseShaderRegister = kStorageImageBaseRegister;
    ranges[3].RegisterSpace = kRegisterSpace;
    ranges[3].OffsetInDescriptorsFromTableStart = kTableStorageImages;

    D3D12_ROOT_PARAMETER params[kRootParamCount]{};
    params[kRootParamConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kRootParamConstants].Constants.ShaderRegister = kRootConstantRegister;
    params[kRootParamConstants].Constants.RegisterSpace = kRootConstantSpace;
    params[kRootParamConstants].Constants.Num32BitValues = kRootConstantDwords;
    params[kRootParamConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[kRootParamTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRootParamTable].DescriptorTable.NumDescriptorRanges = 4;
    params[kRootParamTable].DescriptorTable.pDescriptorRanges = ranges;
    params[kRootParamTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    /* The four samplers are a table of their own over their own heap, not
     * four static samplers. rhi_d3d12_bindings.h carries the reason and the
     * validator message; the short form is that the generated HLSL declares
     * `SamplerState g_samplers[4] : register(s0)`, a shader sampler range of
     * four registers, and D3D12 binds a range of more than one register from
     * a descriptor table range only. */
    D3D12_DESCRIPTOR_RANGE sampler_range{};
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.NumDescriptors = kSamplerCount;
    sampler_range.BaseShaderRegister = kSamplerBaseRegister;
    sampler_range.RegisterSpace = kRegisterSpace;
    sampler_range.OffsetInDescriptorsFromTableStart = 0;
    params[kRootParamSamplers].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRootParamSamplers].DescriptorTable.NumDescriptorRanges = 1;
    params[kRootParamSamplers].DescriptorTable.pDescriptorRanges = &sampler_range;
    params[kRootParamSamplers].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = kRootParamCount;
    rs.pParameters = params;
    rs.NumStaticSamplers = 0;
    rs.pStaticSamplers = nullptr;
    /* The overlay draws from a vertex buffer, so the input assembler stage
     * has to be allowed. Nothing else is. */
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    HRESULT hr = E_NOTIMPL;
    if (m_entries->D3D12SerializeVersionedRootSignature) {
        /* The description is version 1.0 even through the versioned
         * serializer: 1.1's descriptor volatility flags describe promises
         * about when descriptors change, and this backend writes a fresh
         * table for every dispatch and draw, so it can promise nothing and
         * gains nothing. */
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd{};
        vd.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        vd.Desc_1_0 = rs;
        hr = m_entries->D3D12SerializeVersionedRootSignature(&vd, blob.put(), error.put());
    } else if (m_entries->D3D12SerializeRootSignature) {
        hr = m_entries->D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                    blob.put(), error.put());
    } else {
        fatal("d3d12.dll exports neither D3D12SerializeVersionedRootSignature nor "
              "D3D12SerializeRootSignature");
    }
    if (FAILED(hr)) {
        fatal("serializing the root signature failed (HRESULT 0x%08lx, %s): %s",
              (unsigned long)hr, d3d12_hresult_name(hr),
              error ? (const char*)error->GetBufferPointer() : "no diagnostic");
    }
    check(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                        __uuidof(ID3D12RootSignature),
                                        m_root_signature.put_void()),
          "CreateRootSignature");
}

void D3D12Device::create_frames() {
    for (Frame& f : m_frames) {
        check(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               __uuidof(ID3D12CommandAllocator),
                                               f.allocator.put_void()),
              "CreateCommandAllocator");
        check(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          f.allocator.get(), nullptr,
                                          __uuidof(ID3D12GraphicsCommandList),
                                          f.list.put_void()),
              "CreateCommandList");
        /* A command list is created open. Nothing is recorded into it here,
         * so it is closed and begin_command_list reopens it. */
        check(f.list->Close(), "closing the freshly created command list");
    }
}

void D3D12Device::create_dummies() {
    /* Every descriptor of every range has to be a real view: this backend
     * writes all 32 descriptors of a set on every dispatch and draw, because
     * a table with a hole is undefined on resource binding tiers 1 and 2.
     * Two dummy buffers and not one, for the reason rhi_d3d12.h gives. */
    BufferDesc bd;
    bd.size = 256;
    bd.kind = BufferKind::DeviceLocal;
    bd.usage = BufferUsage::Storage | BufferUsage::CopyDst;
    bd.debug_name = "rhi dummy storage buffer";
    m_dummy_buffer = create_buffer(bd);

    bd.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
    bd.debug_name = "rhi dummy uniform buffer";
    m_dummy_uniform_buffer = create_buffer(bd);

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

    /* One submit at startup puts every dummy in the state its descriptor
     * class needs, so no later frame carries a transition for them and the
     * debug layer stays quiet. */
    CommandList* cmd = begin_command_list();
    D3D12CommandList* d = static_cast<D3D12CommandList*>(cmd);
    d->transition(m_dummy_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d->transition(m_dummy_uniform_buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    d->transition(m_dummy_texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                 | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    d->transition(m_dummy_storage_image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    wait(submit(cmd));
}

/* ---- buffers -------------------------------------------------------------- */

Buffer* D3D12Device::create_buffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        fatal("create_buffer with size 0 (%s)", desc.debug_name ? desc.debug_name : "unnamed");
    }
    if (desc.kind == BufferKind::Readback && has(desc.usage, BufferUsage::Storage)) {
        fatal("a readback buffer cannot also be a storage buffer on D3D12: a readback heap "
              "resource is fixed at COPY_DEST and cannot carry a UAV (%s)",
              desc.debug_name ? desc.debug_name : "unnamed");
    }

    Buffer* b = new Buffer();
    b->size = desc.size;
    b->kind = desc.kind;
    b->usage = desc.usage;
    /* A host-visible buffer the shaders read has to live twice: D3D12 forbids
     * ALLOW_UNORDERED_ACCESS on an upload heap, and rhi_d3d12_bindings.h says
     * why the storage buffers are UAVs. The CPU writes the upload copy and
     * bind_storage_buffer copies the bound range into the default-heap
     * shadow, so what the shader reads is what the CPU had written when the
     * bind was recorded. */
    b->shadowed = desc.kind == BufferKind::Upload && has(desc.usage, BufferUsage::Storage);

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0;
    rd.Width = desc.size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES hp{};
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    const bool wants_default = desc.kind == BufferKind::DeviceLocal || b->shadowed;
    if (wants_default) {
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (has(desc.usage, BufferUsage::Storage)) {
            rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        b->state = D3D12_RESOURCE_STATE_COMMON;
        b->tracked = true;
        check(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, b->state,
                                                nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&b->resource)),
              "CreateCommittedResource (default heap buffer)");
    }

    if (desc.kind != BufferKind::DeviceLocal) {
        D3D12_RESOURCE_DESC hd = rd;
        hd.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hhp{};
        hhp.CreationNodeMask = 1;
        hhp.VisibleNodeMask = 1;
        D3D12_RESOURCE_STATES host_state;
        if (desc.kind == BufferKind::Upload) {
            hhp.Type = D3D12_HEAP_TYPE_UPLOAD;
            host_state = D3D12_RESOURCE_STATE_GENERIC_READ;
        } else {
            hhp.Type = D3D12_HEAP_TYPE_READBACK;
            host_state = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        ID3D12Resource* host = nullptr;
        check(m_device->CreateCommittedResource(&hhp, D3D12_HEAP_FLAG_NONE, &hd, host_state,
                                                nullptr, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&host)),
              "CreateCommittedResource (host visible buffer)");
        b->staging = host;
        if (!b->resource) {
            /* Not shadowed: the host resource is the resource. Its state is
             * fixed for its whole life, which is what D3D12 requires of an
             * upload or readback heap. */
            b->resource = host;
            b->state = host_state;
            b->tracked = false;
        }
        /* The mapping lives for the life of the buffer, as rhi.h promises.
         * Upload writes are declared with an empty read range; a readback
         * buffer passes null because the CPU may read all of it. */
        const D3D12_RANGE none{ 0, 0 };
        check(host->Map(0, desc.kind == BufferKind::Upload ? &none : nullptr, &b->mapped),
              "Map (host visible buffer)");
    }

    if (!b->resource) {
        fatal("no resource was created for a %llu byte %s buffer (%s)",
              (unsigned long long)desc.size, heap_name(desc.kind),
              desc.debug_name ? desc.debug_name : "unnamed");
    }
    return b;
}

void D3D12Device::destroy_buffer(Buffer* b) {
    if (!b) return;
    if (b->staging && b->staging != b->resource) b->staging->Release();
    if (b->resource) b->resource->Release();
    delete b;
}

void* D3D12Device::map(Buffer* b) {
    if (!b) fatal("map of a null buffer");
    if (!b->mapped) fatal("map of a device-local buffer; only Upload and Readback are mappable");
    return b->mapped;
}

void D3D12Device::flush_storage_shadow(ID3D12GraphicsCommandList* list, Buffer* b,
                                       uint64_t offset, uint64_t bytes) {
    if (!b || !b->shadowed || bytes == 0) return;
    if (offset + bytes > b->size) bytes = b->size - offset;

    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = b->resource;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy.Transition.StateBefore = b->state;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    if (b->state != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &to_copy);
    list->CopyBufferRegion(b->resource, offset, b->staging, offset, bytes);
    b->state = D3D12_RESOURCE_STATE_COPY_DEST;
}

/* ---- textures ------------------------------------------------------------- */

Texture* D3D12Device::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        fatal("create_texture %ux%u (%s)", desc.width, desc.height,
              desc.debug_name ? desc.debug_name : "unnamed");
    }
    Texture* t = new Texture();
    t->width = desc.width;
    t->height = desc.height;
    t->format = to_dxgi_format(desc.format);
    t->usage = desc.usage;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc.width;
    rd.Height = desc.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = t->format;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (has(desc.usage, TextureUsage::Storage)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (has(desc.usage, TextureUsage::ColorTarget)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    t->state = D3D12_RESOURCE_STATE_COMMON;
    check(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t->state, nullptr,
                                            __uuidof(ID3D12Resource),
                                            reinterpret_cast<void**>(&t->resource)),
          "CreateCommittedResource (texture)");

    if (has(desc.usage, TextureUsage::ColorTarget)) {
        t->rtv = alloc_rtv();
        m_device->CreateRenderTargetView(t->resource, nullptr, rtv_handle(t->rtv));
    }
    return t;
}

Format D3D12Device::texture_format(Texture* t) const {
    if (!t) {
        rt_log_warn("rhi", "texture_format of a null texture; the caller is told Unknown");
        return Format::Unknown;
    }
    switch (t->format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8Unorm;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return Format::BGRA8Unorm;
        case DXGI_FORMAT_R32_UINT:       return Format::R32Uint;
        default: break;
    }
    /* Unknown is not a value any caller can build a pipeline or a readback
     * from, and this is the only place that still knows the DXGI format the
     * texture actually has. Once per distinct format. */
    static bool said_any = false;
    static DXGI_FORMAT said = DXGI_FORMAT_UNKNOWN;
    if (!said_any || t->format != said) {
        said_any = true;
        said = t->format;
        rt_log_warn("rhi", "texture_format of DXGI format %u (%s), which has no rhi Format; "
                           "the caller is told Unknown",
                    (unsigned)t->format, dxgi_format_name(t->format));
    }
    return Format::Unknown;
}

void D3D12Device::destroy_texture(Texture* t) {
    if (!t) return;
    if (t->rtv != UINT32_MAX) free_rtv(t->rtv);
    if (t->owns_resource && t->resource) t->resource->Release();
    delete t;
}

uint32_t D3D12Device::alloc_rtv() {
    if (!m_rtv_free.empty()) {
        const uint32_t i = m_rtv_free.back();
        m_rtv_free.pop_back();
        return i;
    }
    if (m_rtv_next >= kRtvHeapSize) {
        fatal("the render target view heap is full (%u views); the renderer created more "
              "colour targets than this backend was sized for", (unsigned)kRtvHeapSize);
    }
    return m_rtv_next++;
}

void D3D12Device::free_rtv(uint32_t index) { m_rtv_free.push_back(index); }

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::rtv_handle(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * m_rtv_size;
    return h;
}

ID3D12Resource* D3D12Device::scratch(uint64_t bytes) {
    if (m_scratch && m_scratch_bytes >= bytes) return m_scratch.get();
    /* This can be called while a command list is open, and that list may
     * already reference the old buffer, so the old one is retired rather than
     * released; wait_idle frees the retired set once the queue is empty. */
    if (m_scratch) m_retired.push_back(std::move(m_scratch));
    m_scratch_bytes = align_up(bytes, 1u << 20);

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = m_scratch_bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    m_scratch_state = D3D12_RESOURCE_STATE_COMMON;
    check(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, m_scratch_state,
                                            nullptr, __uuidof(ID3D12Resource),
                                            m_scratch.put_void()),
          "CreateCommittedResource (copy scratch)");
    return m_scratch.get();
}

/* ---- pipelines ------------------------------------------------------------ */

ComputePipeline* D3D12Device::create_compute_pipeline(const uint32_t* spirv, size_t words,
                                                      const char* name) {
    const ShaderBytecode cs = d3d12_shader_for_spirv(this, spirv, words, "cs", name);
    if (!m_logged_shader_path) {
        m_logged_shader_path = true;
        rt_log_info("rhi", "D3D12 shaders come from %s",
                    cs.from_dxil ? "the committed DXIL (rhi_shaders_dxil.h)"
                                 : "the committed HLSL, compiled by dxcompiler.dll");
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_root_signature.get();
    pd.CS.pShaderBytecode = cs.bytes.data();
    pd.CS.BytecodeLength = cs.bytes.size();

    ComputePipeline* p = new ComputePipeline();
    const HRESULT hr = m_device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                                            reinterpret_cast<void**>(&p->pso));
    if (FAILED(hr)) {
        delete p;
        report_compute_pipeline_failure(name ? name : "(unnamed)", hr, pd, cs);
    }
    return p;
}

void D3D12Device::destroy_compute_pipeline(ComputePipeline* p) {
    if (!p) return;
    if (p->pso) p->pso->Release();
    delete p;
}

namespace {

/* The overlay's vertex layout, documented in rhi.h: float2 position, float2
 * texture coordinate, one packed RGBA8 colour, 20 bytes.
 *
 * The semantics are TEXCOORD0/1/2 and not POSITION/TEXCOORD/COLOR because
 * that is what SPIRV-Cross names a location decoration in its HLSL output:
 * location N becomes TEXCOORDN. tools/gen_gs_shaders.sh leaves that default
 * alone, so this table and the generated vertex shader agree by construction.
 */
const D3D12_INPUT_ELEMENT_DESC kOverlayInput[3] = {
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,   0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 2, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

void fill_common_graphics_state(D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd) {
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    /* Counter-clockwise front faces and no culling, matching the Vulkan
     * pipeline. With culling off the winding decides nothing, but it is set
     * so the two backends describe the same state. */
    pd.RasterizerState.FrontCounterClockwise = TRUE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.SampleDesc.Count = 1;
}

} // namespace

GraphicsPipeline* D3D12Device::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    const char* name = desc.debug_name ? desc.debug_name : "(unnamed)";
    const ShaderBytecode vs = d3d12_shader_for_spirv(this, desc.vertex_spirv,
                                                     desc.vertex_spirv_words, "vs", name);
    const ShaderBytecode ps = d3d12_shader_for_spirv(this, desc.fragment_spirv,
                                                     desc.fragment_spirv_words, "ps", name);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_root_signature.get();
    pd.VS.pShaderBytecode = vs.bytes.data();
    pd.VS.BytecodeLength = vs.bytes.size();
    pd.PS.pShaderBytecode = ps.bytes.data();
    pd.PS.BytecodeLength = ps.bytes.size();
    pd.InputLayout.pInputElementDescs = kOverlayInput;
    pd.InputLayout.NumElements = 3;
    fill_common_graphics_state(pd);
    pd.RTVFormats[0] = to_dxgi_format(desc.color_format);

    D3D12_RENDER_TARGET_BLEND_DESC& rt = pd.BlendState.RenderTarget[0];
    rt.BlendEnable = desc.blend ? TRUE : FALSE;
    rt.LogicOpEnable = FALSE;
    /* Premultiplied geometry blends with ONE, straight alpha with SRC_ALPHA;
     * the same pair the Vulkan pipeline uses, and the same reason
     * (RT_PGS_OVERLAY_PREMULTIPLIED). */
    rt.SrcBlend = desc.premultiplied ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    GraphicsPipeline* p = new GraphicsPipeline();
    const HRESULT hr = m_device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState),
                                                             reinterpret_cast<void**>(&p->pso));
    if (FAILED(hr)) {
        delete p;
        report_graphics_pipeline_failure(name, hr, pd, vs, ps);
    }
    return p;
}

void D3D12Device::destroy_graphics_pipeline(GraphicsPipeline* p) {
    if (!p) return;
    if (p->pso) p->pso->Release();
    delete p;
}

GraphicsPipeline* D3D12Device::blit_pipeline(DXGI_FORMAT format) {
    for (const BlitPipeline& b : m_blit_pipelines) {
        if (b.format == format) return b.pipeline;
    }
    const ShaderBytecode vs = d3d12_internal_shader(this, "blit.vert", "vs");
    const ShaderBytecode ps = d3d12_internal_shader(this, "blit.frag", "ps");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_root_signature.get();
    pd.VS.pShaderBytecode = vs.bytes.data();
    pd.VS.BytecodeLength = vs.bytes.size();
    pd.PS.pShaderBytecode = ps.bytes.data();
    pd.PS.BytecodeLength = ps.bytes.size();
    /* No input layout: the blit's three vertices come from SV_VertexID. */
    fill_common_graphics_state(pd);
    pd.RTVFormats[0] = format;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    GraphicsPipeline* p = new GraphicsPipeline();
    const HRESULT hr = m_device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState),
                                                             reinterpret_cast<void**>(&p->pso));
    if (FAILED(hr)) {
        delete p;
        char blit_name[96];
        std::snprintf(blit_name, sizeof(blit_name), "present blit (%s)",
                      dxgi_format_name(format));
        report_graphics_pipeline_failure(blit_name, hr, pd, vs, ps);
    }
    BlitPipeline entry;
    entry.format = format;
    entry.pipeline = p;
    m_blit_pipelines.push_back(entry);
    return p;
}

/* ---- submission ----------------------------------------------------------- */

void D3D12Device::allocate_descriptor_set(D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                          D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    Frame& f = m_frames[m_frame_index];
    if (f.descriptor_next >= kSetsPerFrame) {
        fatal("the frame's descriptor ring is exhausted (%u sets); the renderer issued more "
              "dispatches and draws in one frame than the ring was sized for",
              (unsigned)kSetsPerFrame);
    }
    const uint32_t base = (m_frame_index * kSetsPerFrame + f.descriptor_next)
                        * d3d12_bind::kDescriptorsPerSet;
    ++f.descriptor_next;
    *cpu = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    cpu->ptr += (SIZE_T)base * m_descriptor_size;
    *gpu = m_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    gpu->ptr += (UINT64)base * m_descriptor_size;
}

CommandList* D3D12Device::begin_command_list() {
    if (m_recording) fatal("begin_command_list while a command list is still open");
    Frame& f = m_frames[m_frame_index];
    /* The frame's allocator and its slice of the descriptor ring are reused,
     * so everything the previous use of this frame submitted has to have
     * completed first. */
    wait(f.timeline);
    check(f.allocator->Reset(), "ID3D12CommandAllocator::Reset");
    check(f.list->Reset(f.allocator.get(), nullptr), "ID3D12GraphicsCommandList::Reset");
    f.descriptor_next = 0;

    ID3D12DescriptorHeap* heaps[] = { m_descriptor_heap.get(), m_sampler_heap.get() };
    f.list->SetDescriptorHeaps(2, heaps);
    f.list->SetGraphicsRootSignature(m_root_signature.get());
    f.list->SetComputeRootSignature(m_root_signature.get());
    /* The sampler table never changes, so it is bound once here rather than
     * at every draw and dispatch. The two other root parameters are written
     * per draw and per dispatch in rhi_d3d12_cmd.cpp. */
    const D3D12_GPU_DESCRIPTOR_HANDLE samplers =
        m_sampler_heap->GetGPUDescriptorHandleForHeapStart();
    f.list->SetGraphicsRootDescriptorTable(d3d12_bind::kRootParamSamplers, samplers);
    f.list->SetComputeRootDescriptorTable(d3d12_bind::kRootParamSamplers, samplers);

    m_cmd->reset(f.list.get());
    m_recording = true;
    return m_cmd;
}

uint64_t D3D12Device::submit(CommandList* cmd) {
    if (!m_recording || cmd != m_cmd) fatal("submit of a command list that is not open");
    Frame& f = m_frames[m_frame_index];
    check(f.list->Close(), "ID3D12GraphicsCommandList::Close");
    m_recording = false;

    ID3D12CommandList* lists[] = { f.list.get() };
    m_queue->ExecuteCommandLists(1, lists);

    const uint64_t value = ++m_timeline_value;
    check(m_queue->Signal(m_fence.get(), value), "ID3D12CommandQueue::Signal");
    f.timeline = value;
    /* A backbuffer this submit wrote cannot be handed out again until this
     * value is reached; acquire_backbuffer waits on it. */
    if (m_backbuffer_index != UINT32_MAX && m_backbuffer_index < m_backbuffer_timeline.size()
        && m_cmd->touched_swapchain()) {
        m_backbuffer_timeline[m_backbuffer_index] = value;
    }
    m_frame_index = (m_frame_index + 1) % kFrames;
    drain_debug_messages();
    return value;
}

void D3D12Device::wait(uint64_t value) {
    if (value == 0) return;
    const uint64_t completed = m_fence->GetCompletedValue();
    /* UINT64_MAX is what GetCompletedValue returns on a removed device, and
     * it compares as complete against every timeline value: without this the
     * wait would return at once, for the rest of the run, and every frame
     * after would read a resource the GPU never wrote. */
    if (completed == UINT64_MAX) {
        const HRESULT reason = m_device->GetDeviceRemovedReason();
        if (FAILED(reason)) {
            fatal("ID3D12Fence::GetCompletedValue returned UINT64_MAX, which is what a "
                  "fence reports once its device is gone, and GetDeviceRemovedReason says "
                  "0x%08lx (%s). Nothing recovers from a removed device, so the run ends "
                  "here rather than treating every later wait as already complete.",
                  (unsigned long)reason, d3d12_hresult_name(reason));
        }
        /* The device says it is fine, so this is a completed value no submit
         * of this run can have produced. The comparison below treats it as
         * complete and this wait returns at once, which is what the previous
         * code did silently. */
        rt_log_error("rhi", "ID3D12Fence::GetCompletedValue returned UINT64_MAX while the "
                            "device reports no removal; the wait for %llu is treated as "
                            "already satisfied",
                     (unsigned long long)value);
    }
    if (completed >= value) return;
    check(m_fence->SetEventOnCompletion(value, m_fence_event),
          "ID3D12Fence::SetEventOnCompletion");
    const DWORD w = WaitForSingleObject(m_fence_event, INFINITE);
    if (w != WAIT_OBJECT_0) {
        /* An infinite wait returns WAIT_OBJECT_0 or WAIT_FAILED. On the
         * second the fence was never waited on, so everything after this
         * reads memory the GPU may still be writing. */
        rt_log_error("rhi", "the wait for fence value %llu returned %lu (Win32 error %lu) "
                            "instead of being signalled; work this thread is about to read "
                            "may not have completed",
                     (unsigned long long)value, (unsigned long)w,
                     (unsigned long)GetLastError());
    }
    const HRESULT removed = m_device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        /* Not fatal_device_removed: no call returned a code here, the device
         * simply reports itself gone after the wait, so there is only the one
         * HRESULT to print. */
        fatal("the D3D12 device was removed while waiting for submitted work: "
              "GetDeviceRemovedReason 0x%08lx (%s). Nothing recovers from a removed "
              "device, so the run ends here.",
              (unsigned long)removed, d3d12_hresult_name(removed));
    }
}

void D3D12Device::wait_idle() {
    if (!m_queue) return;
    const uint64_t value = ++m_timeline_value;
    check(m_queue->Signal(m_fence.get(), value), "ID3D12CommandQueue::Signal (wait_idle)");
    wait(value);
    /* Nothing on the queue can still be reading a retired scratch buffer.
     * The open list is the exception: gs_native calls wait_idle from inside a
     * command list when it grows an upload buffer, and that list may already
     * have recorded a copy through the scratch this would free. The retired
     * set is left alone until a wait_idle with nothing recording. */
    if (!m_recording) m_retired.clear();
    drain_debug_messages();
}

/* ---- swapchain ------------------------------------------------------------
 *
 * DXGI has no present modes, so rhi.h's three are mapped onto a sync interval
 * and a present flag. What each one actually does is written down in
 * docs/GS_RENDERER.md as well, because none of the three behaves exactly as
 * its Vulkan namesake:
 *
 *   Fifo       SyncInterval 1, no flag. One present per vertical blank, and
 *              Present blocks once the queue is full. This is the Vulkan
 *              FIFO behaviour.
 *   Mailbox    SyncInterval 0, no flag. With a flip-discard swapchain the
 *              compositor takes the most recently completed present and
 *              discards the ones behind it, which is what mailbox means; it
 *              is not identical, because DXGI still ties the change to a
 *              vertical blank while Vulkan's mailbox may not.
 *   Immediate  SyncInterval 0 plus DXGI_PRESENT_ALLOW_TEARING, which needs
 *              the swapchain to carry DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING and
 *              the adapter to report the feature. Where it is unsupported the
 *              mode falls back to mailbox's flags with a log line, because
 *              tearing is the only thing immediate adds.
 *
 * All three flags are decided at Present time and the swapchain carries the
 * tearing flag from creation, so set_present_mode does not rebuild it. That
 * is a difference from the Vulkan backend, where the mode is baked into the
 * swapchain object.
 */

void D3D12Device::create_swapchain(uint32_t width, uint32_t height) {
    if (!m_hwnd) {
        /* Headless. Reached only through acquire_backbuffer's rebuild, which
         * checks m_hwnd first, so arriving here means a window went away
         * under a device that was created with one. */
        rt_log_error("rhi", "create_swapchain with no window handle; this device presents "
                            "to a Win32 window and now has none, so nothing can be shown");
        return;
    }
    RECT client{};
    if (GetClientRect((HWND)m_hwnd, &client)) {
        width = (uint32_t)(client.right - client.left);
        height = (uint32_t)(client.bottom - client.top);
    } else {
        /* The caller's size is used instead. Loud because the two can
         * disagree: a swapchain built at the wrong size presents a stretched
         * or clipped picture and nothing else reports it. */
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_warn("rhi", "GetClientRect failed with Win32 error %lu; the swapchain is "
                               "built at the size the runtime last reported (%ux%u), which "
                               "may not be the window's",
                        (unsigned long)GetLastError(), width, height);
        }
    }
    if (width == 0 || height == 0) {
        /* A minimised window. Not an error: the swapchain is left absent and
         * acquire_backbuffer rebuilds it once the window has a size again.
         * Said once and at warn, because the visible effect is a window that
         * shows nothing, and a run that reaches this at startup and stays
         * there would otherwise look like a renderer that failed silently. */
        if (!m_said_no_client_area) {
            m_said_no_client_area = true;
            rt_log_warn("rhi", "the window's client area is %ux%u, so there is nothing to "
                               "build a swapchain on. Nothing is presented until the window "
                               "has a size; this is what a minimised window looks like.",
                        width, height);
        }
        m_surface_width = 0;
        m_surface_height = 0;
        m_swapchain_dirty = true;
        return;
    }

    m_allow_tearing = false;
    ComPtr<IDXGIFactory5> factory5;
    /* A failure here leaves tearing off, which silently turns present mode
     * immediate into mailbox. Said once: the probe runs again on every
     * swapchain rebuild and the answer does not change within a run. */
    static bool said_tearing_probe = false;
    const HRESULT thr = m_factory->QueryInterface(__uuidof(IDXGIFactory5),
                                                  factory5.put_void());
    if (FAILED(thr)) {
        if (!said_tearing_probe) {
            said_tearing_probe = true;
            rt_log_warn("rhi", "QueryInterface(IDXGIFactory5) failed with HRESULT 0x%08lx "
                               "(%s), so allow-tearing cannot be probed and present mode "
                               "immediate will behave as mailbox. Expected on Windows "
                               "older than 10.",
                        (unsigned long)thr, d3d12_hresult_name(thr));
        }
    } else {
        BOOL tearing = FALSE;
        const HRESULT chr = factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                          &tearing, sizeof(tearing));
        if (FAILED(chr)) {
            if (!said_tearing_probe) {
                said_tearing_probe = true;
                rt_log_warn("rhi", "CheckFeatureSupport(PRESENT_ALLOW_TEARING) failed with "
                                   "HRESULT 0x%08lx (%s); tearing is treated as unsupported "
                                   "and present mode immediate will behave as mailbox",
                            (unsigned long)chr, d3d12_hresult_name(chr));
            }
        } else {
            m_allow_tearing = tearing != FALSE;
        }
    }

    /* R8G8B8A8_UNORM rather than the B8G8R8A8 a Vulkan surface usually hands
     * back: both are flip-model formats, and RGBA is the RHI's own Format
     * order, so readback needs no channel swap. */
    m_swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = m_swapchain_format;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    /* Three buffers with flip discard: two would make Present block on the
     * one the compositor still holds, which is what turns mailbox into fifo. */
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (m_allow_tearing) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    ComPtr<IDXGISwapChain1> chain1;
    check(m_factory->CreateSwapChainForHwnd(m_queue.get(), (HWND)m_hwnd, &sd, nullptr,
                                            nullptr, chain1.put()),
          "IDXGIFactory2::CreateSwapChainForHwnd");
    /* Alt+Enter would put the swapchain into a fullscreen state this backend
     * does not manage; the window is the runtime's business. Not fatal when
     * it fails, but then Alt+Enter is live and the fullscreen transition it
     * causes is not something this backend handles, so it is named. */
    const HRESULT ahr = m_factory->MakeWindowAssociation((HWND)m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(ahr)) {
        rt_log_warn("rhi", "IDXGIFactory::MakeWindowAssociation failed with HRESULT 0x%08lx "
                           "(%s); DXGI keeps its Alt+Enter handling, and the fullscreen "
                           "state it switches to is not one this backend manages",
                    (unsigned long)ahr, d3d12_hresult_name(ahr));
    }
    check(chain1->QueryInterface(__uuidof(IDXGISwapChain3), m_swapchain.put_void()),
          "QueryInterface(IDXGISwapChain3)");

    /* Both of these are the flip model's back pressure. Without them the
     * renderer queues frames the compositor has not shown and the present
     * latency grows without bound, which reads as input lag and nothing else
     * would report it. */
    const HRESULT lhr = m_swapchain->SetMaximumFrameLatency(2);
    if (FAILED(lhr)) {
        rt_log_warn("rhi", "IDXGISwapChain2::SetMaximumFrameLatency(2) failed with HRESULT "
                           "0x%08lx (%s); the swapchain keeps its default queue depth and "
                           "present latency may be higher than asked for",
                    (unsigned long)lhr, d3d12_hresult_name(lhr));
    }
    m_frame_latency = m_swapchain->GetFrameLatencyWaitableObject();
    if (!m_frame_latency) {
        rt_log_warn("rhi", "GetFrameLatencyWaitableObject returned no handle even though "
                           "the swapchain was created with FRAME_LATENCY_WAITABLE_OBJECT; "
                           "acquire_backbuffer has no back pressure to wait on and present "
                           "latency is bounded only by the swapchain's own queue");
    }

    m_backbuffers.clear();
    m_backbuffer_timeline.assign(sd.BufferCount, 0);
    for (UINT i = 0; i < sd.BufferCount; ++i) {
        Texture* t = new Texture();
        check(m_swapchain->GetBuffer(i, __uuidof(ID3D12Resource),
                                     reinterpret_cast<void**>(&t->resource)),
              "IDXGISwapChain::GetBuffer");
        t->format = m_swapchain_format;
        t->width = width;
        t->height = height;
        t->usage = TextureUsage::ColorTarget | TextureUsage::CopySrc | TextureUsage::CopyDst;
        /* A freshly presented buffer is in PRESENT, which is COMMON. */
        t->state = D3D12_RESOURCE_STATE_PRESENT;
        t->owns_resource = true;   /* GetBuffer hands out a reference to release */
        t->swapchain = true;
        t->rtv = alloc_rtv();
        m_device->CreateRenderTargetView(t->resource, nullptr, rtv_handle(t->rtv));
        m_backbuffers.push_back(t);
    }
    m_surface_width = width;
    m_surface_height = height;
    m_swapchain_dirty = false;
    /* Startup and rebuild identity, at info: what was built, how big, and
     * whether tearing is available, so a log from a machine that presents
     * nothing shows whether a swapchain ever existed. */
    rt_log_info("rhi", "D3D12 swapchain: %ux%u, %s, %u buffers, flip discard, tearing %s",
                width, height, dxgi_format_name(m_swapchain_format), (unsigned)sd.BufferCount,
                m_allow_tearing ? "allowed" : "not available");
}

void D3D12Device::destroy_swapchain() {
    for (Texture* t : m_backbuffers) destroy_texture(t);
    m_backbuffers.clear();
    m_backbuffer_timeline.clear();
    if (m_frame_latency) {
        /* GetFrameLatencyWaitableObject hands out a handle the caller owns. */
        CloseHandle(m_frame_latency);
        m_frame_latency = nullptr;
    }
    m_swapchain.reset();
    m_backbuffer_index = UINT32_MAX;
}

void D3D12Device::set_present_mode(PresentMode mode) {
    /* No rebuild: the mode is a Present argument on DXGI, not swapchain
     * state. See the comment above create_swapchain. */
    m_present_mode = mode;
}

void D3D12Device::notify_resize(uint32_t width, uint32_t height) {
    m_surface_width = width;
    m_surface_height = height;
    m_swapchain_dirty = true;
}

bool D3D12Device::acquire_backbuffer(Texture** out) {
    if (out) *out = nullptr;
    m_backbuffer_index = UINT32_MAX;
    /* Every false return below drops a field. The caller draws nothing and
     * returns, so this function is the only place that can say why. */
    if (!m_hwnd) {
        note_present_skipped("this device was created without a window handle, so it has "
                             "no swapchain to acquire from");
        return false;
    }

    if (m_swapchain_dirty || !m_swapchain) {
        wait_idle();
        destroy_swapchain();
        create_swapchain(m_surface_width, m_surface_height);
        if (!m_swapchain) {
            /* create_swapchain has already said which of the two cases this
             * is (a minimised window, or no window at all). */
            note_present_skipped("the swapchain could not be rebuilt at the window's "
                                 "current size");
            return false;
        }
    }

    /* The waitable object is the flip model's back pressure: it becomes
     * signalled when the swapchain is ready for another frame. Without it a
     * fast renderer queues frames the compositor has not shown and the
     * present latency grows without bound. */
    if (m_frame_latency) {
        const DWORD w = WaitForSingleObjectEx(m_frame_latency, 1000, TRUE);
        if (w != WAIT_OBJECT_0) {
            /* The field is still drawn and presented: the wait is pacing, not
             * correctness. A timeout means the compositor has held every
             * buffer for a second, which is a stall worth a line. */
            static bool said = false;
            if (!said) {
                said = true;
                rt_log_warn("rhi", "the swapchain frame latency wait returned %lu (%s) "
                                   "instead of being signalled; the field is presented "
                                   "anyway, unpaced",
                            (unsigned long)w,
                            w == WAIT_TIMEOUT ? "WAIT_TIMEOUT, the compositor has held "
                                                "every buffer for a second"
                            : w == WAIT_IO_COMPLETION ? "WAIT_IO_COMPLETION, an APC ran"
                            : w == WAIT_FAILED ? "WAIT_FAILED" : "unrecognized wait result");
            }
        }
    }

    const uint32_t index = m_swapchain->GetCurrentBackBufferIndex();
    if (index >= m_backbuffers.size()) {
        /* DXGI naming a buffer this backend never took a reference to. The
         * swapchain and the texture list have gone out of step, which no
         * later frame recovers from on its own. */
        rt_log_error("rhi", "GetCurrentBackBufferIndex returned %u and this swapchain has "
                            "%zu backbuffer textures; the field is dropped",
                     (unsigned)index, m_backbuffers.size());
        note_present_skipped("the swapchain handed back a buffer index this backend has no "
                             "texture for");
        return false;
    }
    /* The GPU work that last wrote this buffer has to have finished, because
     * DXGI reuses the resource and D3D12 has no acquire semaphore. */
    wait(m_backbuffer_timeline[index]);
    m_backbuffer_index = index;
    if (out) *out = m_backbuffers[index];
    return true;
}

void D3D12Device::present() {
    if (!m_swapchain) {
        note_present_skipped("there is no swapchain to present to");
        return;
    }
    if (m_backbuffer_index == UINT32_MAX) {
        note_present_skipped("no backbuffer was acquired for this field");
        return;
    }
    /* D3D12 requires a swapchain buffer to be in D3D12_RESOURCE_STATE_PRESENT
     * (which is COMMON, value 0) when IDXGISwapChain::Present is called. The
     * caller's last texture_barrier is what puts it there, and a caller that
     * forgets gets undefined contents on the screen with nothing in the log:
     * on WARP that reads as the right picture, on a discrete driver it need
     * not. The state this backend tracked is checked rather than assumed. The
     * present still goes ahead, because dropping it would turn one wrong
     * frame into a frozen window. */
    if (m_backbuffer_index < m_backbuffers.size()
        && m_backbuffers[m_backbuffer_index]->state != D3D12_RESOURCE_STATE_PRESENT) {
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_error("rhi", "backbuffer %u is in resource state 0x%x and Present "
                                "requires D3D12_RESOURCE_STATE_PRESENT (0x0); the caller's "
                                "last texture_barrier before present did not run or did not "
                                "target Stage::Present. Said once; what reaches the screen "
                                "from here on is undefined.",
                         (unsigned)m_backbuffer_index,
                         (unsigned)m_backbuffers[m_backbuffer_index]->state);
        }
    }
    UINT sync = 1;
    UINT flags = 0;
    if (m_present_mode == PresentMode::Mailbox) {
        sync = 0;
    } else if (m_present_mode == PresentMode::Immediate) {
        sync = 0;
        if (m_allow_tearing) {
            flags |= DXGI_PRESENT_ALLOW_TEARING;
        } else {
            static bool logged = false;
            if (!logged) {
                logged = true;
                rt_log_warn("rhi", "immediate was asked for but this adapter or compositor "
                                   "does not allow tearing; presenting unsynchronised "
                                   "without it");
            }
        }
    }
    const HRESULT hr = m_swapchain->Present(sync, flags);
    if (d3d12_device_lost(hr)) {
        fatal_device_removed("IDXGISwapChain::Present", hr);
    } else if (hr == DXGI_STATUS_OCCLUDED) {
        /* The window is fully covered. Not an error, and not a reason to
         * rebuild: DXGI keeps presenting once it is visible again. It is
         * still a field nobody saw, so it is counted and said once. */
        note_present_skipped("the window is fully covered by another window "
                             "(DXGI_STATUS_OCCLUDED); DXGI presents again once it is "
                             "visible");
        m_backbuffer_index = UINT32_MAX;
        return;
    } else if (FAILED(hr)) {
        check(hr, "IDXGISwapChain::Present");
    }
    note_present_resumed();
    m_backbuffer_index = UINT32_MAX;
}

/* ---- readback -------------------------------------------------------------
 *
 * D3D12 lays a texture into a buffer with each row padded to
 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256 bytes), so the copy lands padded
 * and the rows are packed back together on the CPU here. The one-off cost is
 * a memcpy per row of a picture that is already being read synchronously.
 */
bool D3D12Device::read_texture(Texture* t, std::vector<uint8_t>& out,
                               uint32_t* width, uint32_t* height) {
    if (!t || t->width == 0 || t->height == 0) {
        rt_log_warn("rhi", "read_texture of %s (%ux%u); nothing is read back and the "
                           "caller's buffer is left as it was",
                    t ? "a texture with no picture" : "a null texture",
                    t ? t->width : 0, t ? t->height : 0);
        return false;
    }
    if (t->format != DXGI_FORMAT_R8G8B8A8_UNORM && t->format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        fatal("read_texture of a format this path does not pack (DXGI format %u, %s)",
              (unsigned)t->format, dxgi_format_name(t->format));
    }

    if (t->swapchain) {
        /* This path transitions the texture to COPY_SOURCE and leaves it
         * there, because the caller alone knows what the texture is for next.
         * On a swapchain buffer that is the state Present forbids, so the
         * next present would be undefined. A caller that wants the presented
         * picture records copy_texture_to_buffer inside the present command
         * list, which is what capture_shot does, and keeps the final
         * transition to Stage::Present. */
        fatal("read_texture of a swapchain backbuffer; it would leave the buffer in "
              "COPY_SOURCE and D3D12 requires PRESENT at Present");
    }

    D3D12_RESOURCE_DESC rd = t->resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    m_device->GetCopyableFootprints(&rd, 0, 1, 0, &footprint, &rows, &row_bytes, &total);

    BufferDesc bd;
    bd.size = total;
    bd.kind = BufferKind::Readback;
    bd.usage = BufferUsage::CopyDst;
    bd.debug_name = "rhi readback";
    Buffer* staging = create_buffer(bd);

    CommandList* cmd = begin_command_list();
    D3D12CommandList* d = static_cast<D3D12CommandList*>(cmd);
    d->transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = t->resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging->resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    d->handle()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    wait(submit(cmd));

    const size_t packed_pitch = (size_t)t->width * 4;
    out.resize(packed_pitch * t->height);
    const uint8_t* base = (const uint8_t*)staging->mapped;
    for (uint32_t y = 0; y < t->height; ++y) {
        std::memcpy(out.data() + (size_t)y * packed_pitch,
                    base + (size_t)y * footprint.Footprint.RowPitch, packed_pitch);
    }
    /* A B8G8R8A8 surface comes back with the channels swapped relative to the
     * caller's RGBA contract, exactly as on Vulkan. The swapchain this
     * backend creates is RGBA, so this only runs for a texture the caller
     * asked for in BGRA. */
    if (t->format == DXGI_FORMAT_B8G8R8A8_UNORM) {
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

Device* create_d3d12_device(const DeviceDesc& desc) {
    return new D3D12Device(desc);
}

} // namespace rhi
