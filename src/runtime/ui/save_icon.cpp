/* ui/save_icon.cpp: see save_icon.h. */
#include "save_icon.h"

#include "data_df.h"

#include "../iso/iso9660.h"
#include "../runtime.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

void set_err(char* err, size_t err_len, const char* fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

constexpr const char* kFallbackViewIcon = "boy_blk.ico";

/* Caps on what an outer table entry may claim before it is read, in the
 * same spirit as ui/title_logo.cpp's kMaxArchiveBytes. icon.sys is a fixed
 * 964-byte descriptor and the retail view icon is 95624 bytes; these leave
 * room for a larger one (more shapes, a texture) while keeping a corrupt or
 * hostile outer table from asking for a read the size of DATA.DF itself. */
constexpr uint32_t kMaxIconSysBytes = 64u * 1024;
constexpr uint32_t kMaxIconBytes = 16u * 1024 * 1024;

/* Finds one outer entry and reads it, refusing a declared size past `cap`
 * before any allocation happens. */
bool read_entry(const RtIsoFile& data_df, const char* name, uint32_t cap,
                std::vector<uint8_t>& out, char* err, size_t err_len) {
    uint32_t off = 0, size = 0;
    if (!rt_data_df_find(data_df, name, &off, &size, err, err_len)) return false;
    if (size == 0 || size > cap) {
        set_err(err, err_len, "DATA.DF's %s entry declares %u bytes, outside 1..%u", name, size, cap);
        return false;
    }
    return rt_data_df_read(data_df, off, size, out, err, err_len);
}

} // namespace

bool rt_save_icon_load(RtPs2Icon& icon, RtPs2IconSys& icon_sys, char* err, size_t err_len) try {
    icon = RtPs2Icon();
    icon_sys = RtPs2IconSys();

    RtIsoFile data_df;
    if (!rt_data_df_open(&data_df, err, err_len)) return false;

    std::vector<uint8_t> sys_bytes;
    if (!read_entry(data_df, "icon.sys", kMaxIconSysBytes, sys_bytes, err, err_len)) return false;
    if (!rt_ps2_icon_sys_parse(sys_bytes.data(), sys_bytes.size(), icon_sys, err, err_len)) return false;

    /* icon.sys names its own view icon, so a disc whose save icon is not
     * called boy_blk.ico still works; the fallback is only for a disc whose
     * icon.sys names a file its DATA.DF does not carry. */
    std::string view = icon_sys.view_icon.empty() ? kFallbackViewIcon : icon_sys.view_icon;
    std::vector<uint8_t> icon_bytes;
    if (!read_entry(data_df, view.c_str(), kMaxIconBytes, icon_bytes, err, err_len)) {
        if (view == kFallbackViewIcon) return false;
        rt_log("ui", "save icon: icon.sys names '%s', which DATA.DF does not give (%s); trying '%s'",
            view.c_str(), err, kFallbackViewIcon);
        view = kFallbackViewIcon;
        if (!read_entry(data_df, view.c_str(), kMaxIconBytes, icon_bytes, err, err_len)) return false;
    }
    if (!rt_ps2_icon_parse(icon_bytes.data(), icon_bytes.size(), icon, err, err_len)) {
        set_err(err, err_len, "DATA.DF's '%s' does not parse as a ps2 icon", view.c_str());
        return false;
    }
    return true;
} catch (const std::exception& e) {
    icon = RtPs2Icon();
    icon_sys = RtPs2IconSys();
    set_err(err, err_len, "save icon load threw: %s", e.what());
    return false;
} catch (...) {
    icon = RtPs2Icon();
    icon_sys = RtPs2IconSys();
    set_err(err, err_len, "save icon load threw a non-standard exception");
    return false;
}

bool rt_save_icon_build(uint32_t size_px, RtPs2IconImage& out, char* err, size_t err_len) try {
    out = RtPs2IconImage();
    const auto t0 = std::chrono::steady_clock::now();

    RtPs2Icon icon;
    RtPs2IconSys icon_sys;
    if (!rt_save_icon_load(icon, icon_sys, err, err_len)) return false;
    if (!rt_ps2_icon_render(icon, icon_sys, size_px, 0, out, err, err_len)) return false;

    rt_log("ui", "save icon: built %ux%u in %.1f ms", out.width, out.height, ms_since(t0));
    return true;
} catch (const std::exception& e) {
    out = RtPs2IconImage();
    set_err(err, err_len, "save icon build threw: %s", e.what());
    rt_log("ui", "save icon: build threw (%s); the window keeps its default icon", e.what());
    return false;
} catch (...) {
    out = RtPs2IconImage();
    set_err(err, err_len, "save icon build threw a non-standard exception");
    rt_log("ui", "save icon: build threw a non-standard exception; the window keeps its default"
                " icon");
    return false;
}
