/* host/mouse_names.h: the names the mouse binding strings in
 * input.mouse.<slot> are written with.
 *
 * SDL has no name lookup for mouse buttons the way SDL_GetScancodeName and
 * the gamepad mapping strings cover the other two devices, so these names
 * are ours. That is the whole reason this table exists, and it is why the
 * header pulls in no SDL type: the settings layer, which never links SDL,
 * has to be able to read and write these strings, and host/input.cpp maps
 * the enum onto SDL_BUTTON_* and the wheel event on its own side.
 *
 * Lookup is case-insensitive so a hand-edited "Left" resolves the same way
 * SDL_GetScancodeFromName resolves "f1", which is also the comparison
 * settings.cpp's duplicate rules use.
 *
 * Runtime-internal, like host/settings.h: NOT part of the ABI contract
 * (include/recomp_*.h).
 */
#ifndef ICORECOMP_HOST_MOUSE_NAMES_H
#define ICORECOMP_HOST_MOUSE_NAMES_H

#include <cctype>
#include <cstddef>
#include <string>

/* Order is the order of kMouseInputNames below; nothing persists the
 * numeric value, so it is free to change with the table. */
enum RtMouseInput {
    RT_MOUSE_LEFT,
    RT_MOUSE_RIGHT,
    RT_MOUSE_MIDDLE,
    RT_MOUSE_X1,
    RT_MOUSE_X2,
    RT_MOUSE_WHEEL_UP,
    RT_MOUSE_WHEEL_DOWN,
    RT_MOUSE_INPUT_COUNT
};

/* Lower case in the table: rt_mouse_input_name() is what the settings file
 * is written with, and the compiled-in mouse defaults in settings.cpp are
 * spelled by calling it, so there is one copy of each spelling. */
constexpr const char* kMouseInputNames[RT_MOUSE_INPUT_COUNT] = {
    "left",
    "right",
    "middle",
    "x1",
    "x2",
    "wheelup",
    "wheeldown",
};

/* The name for one input, or "" for a value outside the enum. */
constexpr const char* rt_mouse_input_name(RtMouseInput in) {
    return (in >= 0 && in < RT_MOUSE_INPUT_COUNT) ? kMouseInputNames[(int)in] : "";
}

/* Resolves a stored binding string. Returns false, leaving *out alone, for
 * an empty string (a deliberately unbound mouse slot, which is the shipped
 * default for most of them) and for anything not in the table. */
inline bool rt_mouse_input_from_name(const std::string& name, RtMouseInput* out) {
    if (name.empty()) return false;
    for (int i = 0; i < RT_MOUSE_INPUT_COUNT; ++i) {
        const char* candidate = kMouseInputNames[i];
        size_t k = 0;
        for (; k < name.size() && candidate[k]; ++k) {
            if (std::tolower((unsigned char)name[k]) != (unsigned char)candidate[k]) break;
        }
        if (k == name.size() && !candidate[k]) {
            if (out) *out = (RtMouseInput)i;
            return true;
        }
    }
    return false;
}

#endif /* ICORECOMP_HOST_MOUSE_NAMES_H */
