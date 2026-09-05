/* ui/ui_achievements_model.cpp: the "achievements" data model, the menu's
 * Achievements tab and the unlock toast.
 *
 * One model, read by two documents: the Achievements section inside
 * ui/menu.rml (a nested data-model="achievements" div inside the settings
 * document, which is why this model has to exist before menu.rml is loaded)
 * and ui/achievement_toast.rml, which is its own always-loaded document the
 * way the fps readout is.
 *
 * Everything shown comes from guest/achievements.h. Nothing here reads guest
 * memory, keeps a copy of the trophy set, or decides when something is
 * unlocked; this file turns the module's state into strings and shows and
 * hides one document, on the UI clock, at the field boundary, like every
 * other file in this directory.
 *
 * Two states are deliberately distinguished in the table. A trophy that is
 * locked because the player has not earned it reads "Locked". A trophy that
 * nothing in this port can currently unlock, because its progress bit has
 * not been matched yet (guest/ico_syms.h, the empty trophy bit table), says
 * so instead. Showing the second as the first would be telling the player
 * to go and earn something that cannot fire.
 */
#include "ui.h"

#ifdef ICORECOMP_UI

#include "ui_internal.h"

#include "../guest/achievements.h"
#include "../guest/ico_syms.h"
#include "../runtime.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <cstdio>
#include <string>
#include <vector>

namespace rtui {

namespace {

/* One row of the Achievements tab. Strings, because the document does no
 * arithmetic and RmlUi's data bindings are simplest that way. */
struct UiTrophyRow {
    std::string tier;
    std::string name;
    std::string description;
    /* "Unlocked", "Locked", or the unresolved note's short form. */
    std::string state;
    /* The unlock time, or empty. */
    std::string when;
    /* True for a row nothing in this port can unlock yet; the document shows
     * the long note on it. */
    bool unresolved = false;
    bool unlocked = false;
};

struct AchievementsModel {
    std::vector<UiTrophyRow> trophies;
    /* "3 of 16 unlocked". */
    std::string summary;
    /* "played 1:23:45, 2 runs, 0 game overs". */
    std::string counters;
    /* One line naming what is not resolved, or empty when everything is. */
    std::string caveat;

    /* The toast document reads these three. */
    std::string toast_name;
    std::string toast_tier;
    std::string toast_description;
};

AchievementsModel g_a;
Rml::DataModelHandle g_model;
bool g_model_valid = false;

/* What the table was last built from, so the rebuild happens when something
 * changes and not once per field. The signature is cheap and total: the
 * unlock time of every trophy, which changes on an unlock and on nothing
 * else, plus the counters line. */
std::string g_signature;

/* The trophy currently in the toast, or -1. */
int g_toast_trophy = -1;

std::string tier_of(int trophy) {
    return rt_trophy_tier_name(rt_trophy_info(trophy).tier);
}

/* "1:23:45" from milliseconds. Hours are not capped: a long run should read
 * as a long run rather than wrap. */
std::string duration(uint64_t ms) {
    const uint64_t total = ms / 1000;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu", (unsigned long long)(total / 3600),
        (unsigned long long)((total / 60) % 60), (unsigned long long)(total % 60));
    return buf;
}

std::string build_signature() {
    std::string s;
    for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
        const RtTrophyStatus& st = rt_achievements_status(t);
        s += st.unlocked ? '1' : '0';
        s += st.condition_located ? 'L' : '-';
        s += st.unlocked_at;
        s += ';';
    }
    const RtAchievementCounters& c = rt_achievements_counters();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%llu/%u/%u/%u", (unsigned long long)c.playtime_ms, c.runs,
        c.game_overs, c.clear_count);
    s += buf;
    return s;
}

void rebuild() {
    g_a.trophies.clear();
    g_a.trophies.reserve(RT_TROPHY_COUNT);
    int unlocked = 0;
    int unresolved = 0;

    for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
        const RtTrophyInfo& info = rt_trophy_info(t);
        const RtTrophyStatus& st = rt_achievements_status(t);
        UiTrophyRow row;
        row.tier = tier_of(t);
        row.name = info.name;
        row.unlocked = st.unlocked;
        row.unresolved = !st.unlocked && !st.condition_located;
        /* A hidden trophy keeps its description until it is earned, which is
         * how the public list presents it. */
        row.description = (info.hidden && !st.unlocked) ? std::string("Hidden until unlocked.")
                                                        : std::string(info.description);
        if (st.unlocked) {
            row.state = "Unlocked";
            row.when = st.unlocked_at;
            ++unlocked;
        } else if (row.unresolved) {
            row.state = "Unavailable";
            ++unresolved;
        } else {
            row.state = "Locked";
        }
        g_a.trophies.push_back(std::move(row));
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d of %d unlocked", unlocked, (int)RT_TROPHY_COUNT);
    g_a.summary = buf;

    const RtAchievementCounters& c = rt_achievements_counters();
    /* The playtime term is dropped where the clock cannot run. On a target
     * whose gameplay layout id is not known the observer never accrues
     * playtime, by design; rendering that as "played 0:00:00" would read as
     * a measurement of zero rather than as a value this build does not
     * count. */
    const bool counts_playtime = RT_ICO_LAYOUT_GAMEPLAY != RT_ICO_SYM_UNKNOWN;
    if (counts_playtime) {
        std::snprintf(buf, sizeof(buf), "played %s, %u run%s, %u game over%s",
            duration(c.playtime_ms).c_str(),
            c.runs, c.runs == 1 ? "" : "s", c.game_overs, c.game_overs == 1 ? "" : "s");
    } else {
        std::snprintf(buf, sizeof(buf), "%u run%s, %u game over%s",
            c.runs, c.runs == 1 ? "" : "s", c.game_overs, c.game_overs == 1 ? "" : "s");
    }
    g_a.counters = buf;

    std::string caveat;
    if (unresolved > 0) {
        std::snprintf(buf, sizeof(buf), "%d of these have no condition located in this port yet;"
                                        " see docs/ACHIEVEMENTS.md", unresolved);
        caveat = buf;
    }
    if (!counts_playtime) {
        if (!caveat.empty()) caveat += " ";
        caveat += "Playtime is not counted on this build: the gameplay layout id is not known"
                  " for this target.";
    }
    g_a.caveat = caveat;
}

void refresh_toast_strings(int trophy) {
    const RtTrophyInfo& info = rt_trophy_info(trophy);
    g_a.toast_name = info.name;
    g_a.toast_tier = rt_trophy_tier_name(info.tier);
    /* The description, hidden trophy or not: the toast is the moment of
     * unlocking, and a hidden trophy stops being hidden then. */
    g_a.toast_description = info.description;
}

} // namespace

bool achievements_model_init(Rml::Context* context, const std::string& ui_dir) {
    Rml::DataModelConstructor c = context->CreateDataModel("achievements");
    if (!c) {
        rt_log_warn("ui", "Context::CreateDataModel(\"achievements\") failed; the Achievements tab"
                          " and the unlock toast are disabled");
        return false;
    }

    /* The struct and the array of it before the Bind, the order
     * ui_settings_model.cpp's binding tables use. */
    if (Rml::StructHandle<UiTrophyRow> row = c.RegisterStruct<UiTrophyRow>()) {
        row.RegisterMember("tier", &UiTrophyRow::tier);
        row.RegisterMember("name", &UiTrophyRow::name);
        row.RegisterMember("description", &UiTrophyRow::description);
        row.RegisterMember("state", &UiTrophyRow::state);
        row.RegisterMember("when", &UiTrophyRow::when);
        row.RegisterMember("unresolved", &UiTrophyRow::unresolved);
        row.RegisterMember("unlocked", &UiTrophyRow::unlocked);
    } else {
        rt_log_warn("ui", "RegisterStruct<UiTrophyRow> failed; the Achievements tab is disabled");
        return false;
    }
    if (!c.RegisterArray<std::vector<UiTrophyRow>>()) {
        rt_log_warn("ui", "RegisterArray<vector<UiTrophyRow>> failed; the Achievements tab is"
                          " disabled");
        return false;
    }

    c.Bind("trophies", &g_a.trophies);
    c.Bind("summary", &g_a.summary);
    c.Bind("counters", &g_a.counters);
    c.Bind("caveat", &g_a.caveat);
    c.Bind("toast_name", &g_a.toast_name);
    c.Bind("toast_tier", &g_a.toast_tier);
    c.Bind("toast_description", &g_a.toast_description);

    g_model = c.GetModelHandle();
    g_model_valid = true;

    /* Filled before any document reads it: menu.rml binds its views at load
     * time and the caller loads it right after this returns. */
    rebuild();
    g_signature = build_signature();

    /* After every Bind, for the same reason ui_menu_cursor_init loads its
     * document last. A failure here costs the toast, not the tab. */
    const std::string path = ui_dir + "/achievement_toast.rml";
    g_ui.toast = context->LoadDocument(path);
    if (!g_ui.toast) {
        rt_log_warn("ui", "document %s failed to load; unlocks are logged but not shown",
            path.c_str());
    }
    return true;
}

void achievements_model_refresh() {
    if (!g_model_valid) return;
    rebuild();
    g_signature = build_signature();
    g_model.DirtyAllVariables();
}

void achievements_model_tick() {
    if (!g_model_valid) return;

    /* The toast, on the UI clock. Polling is what starts a toast's four
     * seconds (guest/achievements.h), so this call has to happen whether the
     * menu is up or not, and before the tick's "nothing is up" early-out,
     * which counts g_ui.toast_visible. */
    int trophy = -1;
    const double now = g_ui.system ? g_ui.system->GetElapsedTime() : 0.0;
    const bool want = rt_achievements_poll_toast(now, &trophy) && g_ui.toast != nullptr;
    if (want && trophy != g_toast_trophy) {
        g_toast_trophy = trophy;
        refresh_toast_strings(trophy);
        g_model.DirtyAllVariables();
    }
    if (want != g_ui.toast_visible) {
        g_ui.toast_visible = want;
        if (g_ui.toast) {
            if (want) {
                /* No focus and no modality: the toast must not take the
                 * keyboard from the menu or from the game. */
                g_ui.toast->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            } else {
                g_ui.toast->Hide();
                g_toast_trophy = -1;
            }
        }
    }

    /* The table, only while the menu is actually showing it, and only when
     * something changed. */
    if (!g_ui.visible) return;
    std::string sig = build_signature();
    if (sig == g_signature) return;
    g_signature = std::move(sig);
    rebuild();
    g_model.DirtyAllVariables();
}

} // namespace rtui

#endif /* ICORECOMP_UI */
