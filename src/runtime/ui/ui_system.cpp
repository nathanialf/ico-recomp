/* ui/ui_system.cpp: RmlUi's SystemInterface on the runtime's own services.
 *
 * Time comes from steady_clock rather than the guest's virtual clock: RmlUi
 * uses it for animations and double-click timing, both of which belong to
 * the person at the keyboard, not to the emulated timeline.
 *
 * JoinPath is the base class default (RmlUi's own relative-path joining,
 * which is what resolves a document's stylesheet references against the
 * absolute document path ui.cpp hands it) with one addition: the "logo:"
 * texture scheme the overlay renderer serves is passed through untouched
 * instead of being treated as a file next to the document.
 */
#include "ui.h"

#ifdef ICORECOMP_UI

#include "ui_internal.h"

#include "../runtime.h"

#ifdef ICORECOMP_PGS_SDL
#include <SDL3/SDL.h>
#endif

namespace rtui {

double UiSystemInterface::GetElapsedTime() {
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - m_start;
    return elapsed.count();
}

bool UiSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    const char* level = "Log";
    switch (type) {
    case Rml::Log::LT_ERROR:   level = "Error"; break;
    case Rml::Log::LT_ASSERT:  level = "Assert"; break;
    case Rml::Log::LT_WARNING: level = "Warning"; break;
    case Rml::Log::LT_INFO:    level = "Info"; break;
    case Rml::Log::LT_DEBUG:   level = "Debug"; break;
    case Rml::Log::LT_ALWAYS:
    default:                   level = "Always"; break;
    }
    /* Every level goes to the log, unfiltered: an RCSS or RML mistake shows
     * up as a warning from deep inside RmlUi and nowhere else. Returning
     * true continues execution (false would break into the debugger). */
    rt_log("ui", "RmlUi %s: %s", level, message.c_str());
    return true;
}

void UiSystemInterface::JoinPath(Rml::String& translated_path, const Rml::String& document_path,
                                 const Rml::String& path) {
    /* ui_render.cpp's LoadTexture answers this scheme out of memory; there is
     * no file to resolve. Without this the default would either mangle it or
     * pass it through by accident (its Windows drive-letter rule), and the
     * renderer would be relying on that accident. */
    if (path.compare(0, sizeof(kLogoScheme) - 1, kLogoScheme) == 0) {
        translated_path = path;
        return;
    }
    Rml::SystemInterface::JoinPath(translated_path, document_path, path);
}

void UiSystemInterface::SetMouseCursor(const Rml::String& /*cursor_name*/) {
    /* No cursor changes in v1. The menu is a static document; nothing in it
     * asks for a resize or text caret cursor yet. */
}

#ifdef ICORECOMP_PGS_SDL

void UiSystemInterface::SetClipboardText(const Rml::String& text) {
    if (!SDL_SetClipboardText(text.c_str())) {
        rt_log("ui", "SDL_SetClipboardText failed: %s", SDL_GetError());
    }
}

void UiSystemInterface::GetClipboardText(Rml::String& text) {
    char* raw = SDL_GetClipboardText();
    if (!raw) {
        text.clear();
        return;
    }
    text = raw;
    SDL_free(raw);
}

#else /* !ICORECOMP_PGS_SDL */

/* No SDL means no window either, so rt_ui_init already refused to bring the
 * UI up; these exist only so the class is complete. */
void UiSystemInterface::SetClipboardText(const Rml::String&) {}
void UiSystemInterface::GetClipboardText(Rml::String& text) { text.clear(); }

#endif /* ICORECOMP_PGS_SDL */

} // namespace rtui

#endif /* ICORECOMP_UI */
