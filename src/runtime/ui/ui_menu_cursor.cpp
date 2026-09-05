/* ui/ui_menu_cursor.cpp: the drawn cursor for the game's own menus.
 *
 * While the pointer owns the mouse (guest/menu_nav.h) relative mouse mode
 * stays on, which means the OS cursor is hidden: there is nothing on screen
 * to point with unless the overlay draws it. This file is that drawing. All
 * of the state lives in guest/menu_nav.cpp; this one reads
 * rt_guest_menu_cursor() and turns it into two percentage strings for
 * ui/cursor.rml, so the guest module has no RmlUi dependency.
 *
 * Its own document and its own data model, shown whenever the pointer has
 * the mouse and the last input device was the keyboard and mouse.
 *
 * It is also shown only while the player is actually on a mouse
 * (rt_input_last_device(), host/input.h). An arrow drawn over a menu the
 * player is walking with a pad is an arrow nobody can move, so a pad press
 * hides it and the next mouse motion brings it back.
 *
 * What it draws is the letter I of the game's own wordmark, cut out of the
 * title image the launcher built from the disc, turned to the angle a mouse
 * cursor is drawn at and outlined (ui/cursor_image.h). That image is built
 * here, from ui_menu_cursor_build_from_logo(), which the launcher calls the
 * moment it publishes a title image; when there is none, because no disc
 * gave one or the launcher was never shown, the document keeps the arrow
 * built out of borders in ui/style/base.rcss. One log line says which of the
 * two a run is using.
 *
 * The arrow is placed inside a container that is the presented scanout
 * rectangle, the same space guest/menu_nav.cpp hit-tests in, so the drawn
 * tip and the hit test are the same point in the picture. The whole document is
 * pointer-events: none: the mouse buttons belong to guest/menu_nav.cpp while
 * the pointer has them.
 *
 * Like every other file here, the refresh runs from rt_ui_tick at the field
 * boundary and nothing here writes settings or files.
 */
#include "ui.h"

#ifdef ICORECOMP_UI

#include "ui_internal.h"

#include "../guest/menu_nav.h"
#include "../host/input.h"
#include "../host/mouse.h"
#include "../runtime.h"
#include "cursor_image.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace rtui {

namespace {

struct CursorModel {
    /* The presented scanout rectangle, in percent of the surface. */
    std::string present_left = "0%", present_top = "0%";
    std::string present_width = "100%", present_height = "100%";

    /* The cursor point, in percent of that rectangle. Both the image and the
     * fallback arrow hang off a zero-sized box placed here. */
    std::string cursor_left = "50%", cursor_top = "50%";

    /* True once an image has been cut out of the title logo and published.
     * The document shows the image on this and the arrow built out of
     * borders on its negation. */
    bool image_cursor = false;
    /* The image's source string. It carries the size and a counter, because
     * RmlUi caches a texture by its source and never re-asks for one it has
     * already loaded; a re-cut at a new window scale therefore needs a name
     * it has not seen. */
    std::string cursor_src;
    /* The image's own box, in dp, and where its top left corner goes
     * relative to the cursor point: minus the hotspot, so the hotspot pixel
     * lands on the point. */
    std::string image_left = "0dp", image_top = "0dp";
    std::string image_width = "0dp", image_height = "0dp";
};

CursorModel g_c;
Rml::DataModelHandle g_model;
bool g_model_valid = false;

/* The density the published image was cut at, so a window-scale change can
 * be noticed and the glyph re-cut at the new one rather than resampled on
 * screen, and a counter that gives each cut its own source string. */
float g_image_ratio = 0.0f;
unsigned g_image_generation = 0;

std::string percent(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f%%", double(v) * 100.0);
    return buf;
}

/* A length in real pixels written as the dp the document has to lay it out
 * at. The image is cut at the context's density, so its pixels divided by
 * that density are its dp. */
std::string dp(double pixels, float ratio) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3fdp", pixels / double(ratio > 0.0f ? ratio : 1.0f));
    return buf;
}

/* Places the container on the presented scanout rectangle. False when there
 * is no such rectangle: nothing has been presented yet, or this field
 * presented no scanout image and the library reported the size as zero
 * (gs/gs_parallel_api.h). The previous placement is left alone rather than
 * collapsed onto the top-left corner; the caller hides the cursor for that
 * field instead of drawing it on a stale rectangle. */
bool refresh_present_rect() {
    int32_t x = 0, y = 0, w = 0, h = 0, bb_w = 0, bb_h = 0;
    backend_present_rect(&x, &y, &w, &h, &bb_w, &bb_h);
    if (w <= 0 || h <= 0 || bb_w <= 0 || bb_h <= 0) return false;
    g_c.present_left = percent(float(x) / float(bb_w));
    g_c.present_top = percent(float(y) / float(bb_h));
    g_c.present_width = percent(float(w) / float(bb_w));
    g_c.present_height = percent(float(h) / float(bb_h));
    return true;
}

/* Cuts the glyph out of `rgba` at `ratio` and publishes it under the cursor
 * scheme. Returns false, having logged, and leaves whatever was published
 * before in place: a re-cut that fails keeps the cursor that was working
 * rather than dropping back to the arrow. */
bool cut_cursor(const uint8_t* rgba, uint32_t width, uint32_t height, float ratio) {
    RtCursorImage img;
    char err[256];
    if (!rt_cursor_image_build(rgba, width, height, ratio, img, err, sizeof(err))) {
        rt_log_warn("ui", "menu cursor: nothing could be cut out of the %ux%u title image (%s)", width,
            height, err);
        return false;
    }
    if (!ui_render_set_image(kCursorScheme, img.rgba.data(), img.width, img.height)) return false;

    char src[64];
    std::snprintf(src, sizeof(src), "%s%ux%u.%u", kCursorScheme, img.width, img.height,
        ++g_image_generation);
    /* The name being left has to be released or each re-cut strands a whole
     * texture: RmlUi's texture database holds an entry per source for the
     * life of the context and nothing else drops one. The same reasoning,
     * and the same call, as refresh_logo_source() in ui_launcher.cpp. */
    if (!g_c.cursor_src.empty() && g_c.cursor_src != src) Rml::ReleaseTexture(g_c.cursor_src);
    g_c.cursor_src = src;
    g_c.image_width = dp(double(img.width), ratio);
    g_c.image_height = dp(double(img.height), ratio);
    g_c.image_left = dp(-double(img.hotspot_x), ratio);
    g_c.image_top = dp(-double(img.hotspot_y), ratio);
    g_c.image_cursor = true;
    g_image_ratio = ratio;
    if (g_model_valid) g_model.DirtyAllVariables();

    rt_log_info("ui", "menu cursor: the logo's I, %ux%u px, hotspot (%u,%u), tip at the %s", img.width,
        img.height, img.hotspot_x, img.hotspot_y, img.tip_at_top ? "top" : "bottom");
    if (img.light_outline) {
        rt_log_info("ui", "menu cursor: the glyph's opaque pixels are dark, so its outline is light");
    }
    return true;
}

/* Re-cuts the glyph when the window scale has moved, from the title image
 * the renderer is still holding, so the cursor is never a resampled copy of
 * a cut made for another density. Cheap and rare: one resize, one cut. */
void sync_cursor_scale() {
    if (!g_c.image_cursor) return;
    const float ratio = ui_density_ratio();
    if (std::fabs(ratio - g_image_ratio) < 0.001f) return;
    /* One attempt per density: a cut that fails must not be retried every
     * field for as long as the window stays that size. */
    static float failed_ratio = 0.0f;
    if (std::fabs(ratio - failed_ratio) < 0.001f) return;
    uint32_t logo_w = 0, logo_h = 0;
    const uint8_t* logo = ui_render_image_bytes(kLogoScheme, &logo_w, &logo_h);
    if (!logo) {
        /* The title image is published for the life of the process, so this
         * is only reachable if that ever stops being true. */
        g_image_ratio = ratio;
        return;
    }
    rt_log_info("ui", "menu cursor: window scale moved from %.2f to %.2f; re-cutting the glyph",
        double(g_image_ratio), double(ratio));
    if (cut_cursor(logo, logo_w, logo_h, ratio)) {
        failed_ratio = 0.0f;
    } else {
        failed_ratio = ratio;
    }
}

/* The image being published says nothing about the upload: that happens
 * inside RmlUi's texture load and only the renderer sees the answer. A
 * failed upload would leave the document showing an image element over no
 * texture, with the arrow switched off by image_cursor, which is nothing at
 * all where the cursor should be. */
void poll_cursor_upload() {
    if (!ui_render_take_image_upload_failure(kCursorScheme)) return;
    if (!g_c.image_cursor) return;
    rt_log_warn("ui", "menu cursor: the image could not be uploaded; back to the drawn arrow");
    g_c.image_cursor = false;
    if (g_model_valid) g_model.DirtyAllVariables();
}

} // namespace

void ui_menu_cursor_build_from_logo(const uint8_t* rgba, uint32_t width, uint32_t height) {
    cut_cursor(rgba, width, height, ui_density_ratio());
}

bool ui_menu_cursor_init(Rml::Context* context, const std::string& ui_dir) {
    Rml::DataModelConstructor c = context->CreateDataModel("menucursor");
    if (!c) {
        rt_log_warn("ui", "Context::CreateDataModel(\"menucursor\") failed; the menu pointer has no"
                     " drawn cursor");
        return false;
    }

    c.Bind("present_left", &g_c.present_left);
    c.Bind("present_top", &g_c.present_top);
    c.Bind("present_width", &g_c.present_width);
    c.Bind("present_height", &g_c.present_height);
    c.Bind("cursor_left", &g_c.cursor_left);
    c.Bind("cursor_top", &g_c.cursor_top);
    c.Bind("image_cursor", &g_c.image_cursor);
    c.Bind("cursor_src", &g_c.cursor_src);
    c.Bind("image_left", &g_c.image_left);
    c.Bind("image_top", &g_c.image_top);
    c.Bind("image_width", &g_c.image_width);
    c.Bind("image_height", &g_c.image_height);

    g_model = c.GetModelHandle();
    g_model_valid = true;

    /* After every Bind, never before one: a document binds its views while
     * it is parsed and reads the model at its first layout. */
    const std::string path = ui_dir + "/cursor.rml";
    g_ui.cursor = context->LoadDocument(path);
    if (!g_ui.cursor) {
        rt_log_warn("ui", "document %s failed to load; the menu pointer has no drawn cursor",
               path.c_str());
        return false;
    }
    return true;
}

void ui_menu_cursor_tick() {
    if (!g_ui.cursor || !g_model_valid) return;

    /* Shown while the pointer owns the mouse, the player is on a mouse, and
     * there is a position to point with. The capture term is what keeps this
     * from being a second cursor: relative mode is what hides the OS cursor,
     * and with it off (mouse look off, or the menu over the game's
     * menu) the system is drawing a cursor at that same point already.
     *
     * The device term (host/input.h) is the one that comes and goes during a
     * menu: pressing a pad button hides the arrow, moving the mouse brings it
     * back. Nothing else changes with it. guest/menu_nav.cpp keeps the cursor
     * where it was and keeps hit testing with it, so the arrow reappears
     * where the player left it rather than jumping to the item the pad
     * walked to. */
    float nx = 0.0f, ny = 0.0f;
    /* refresh_present_rect() is part of the test, not a side effect after
     * it: it is false on a field that presented no scanout image, where the
     * library reports an empty rectangle (gs/gs_parallel_api.h) and there is
     * no picture to place the arrow on. Drawing it at the last field's
     * rectangle would put it somewhere the guest never drew, so the arrow is
     * hidden for that field instead. */
    const bool want = rt_guest_menu_wants_mouse() && rt_mouse_captured() &&
                      rt_input_last_device() == RT_INPUT_DEVICE_KBM &&
                      rt_guest_menu_cursor(&nx, &ny) && refresh_present_rect();
    poll_cursor_upload();
    if (want != g_ui.cursor_visible) {
        g_ui.cursor_visible = want;
        if (want) {
            /* One line, the first time a cursor is actually drawn, saying
             * which of the two it is. The image's own line was printed when
             * it was cut; this covers the run that has no image, which is
             * every run that never showed the launcher and every disc that
             * gave no title image. */
            static bool said = false;
            if (!said && !g_c.image_cursor) {
                said = true;
                rt_log_info("ui", "menu cursor: no logo image; drawn arrow");
            }
            /* No focus: this document is not interactive and must not take
             * the keyboard away from a menu over it. */
            g_ui.cursor->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        } else {
            g_ui.cursor->Hide();
        }
    }
    if (!g_ui.cursor_visible) return;

    sync_cursor_scale();
    g_c.cursor_left = percent(nx);
    g_c.cursor_top = percent(ny);
    g_model.DirtyAllVariables();
}

} // namespace rtui

#endif /* ICORECOMP_UI */
