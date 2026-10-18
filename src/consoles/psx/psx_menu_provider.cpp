// psx_menu_provider.cpp -- recomp-ui as a psxrecomp in-game menu provider
// (recomp_ui_psx_menu.h): the menu model and its wiring to PsxMenuHost.
// Every value shown comes from the host and every change goes back through
// it, so the runtime's policies (live apply, restart bits, pause refusal in
// netplay, tray, exits) are the same ones any other provider gets.
#if defined(__has_include)
#if __has_include("psx_menu_host.h")
#define RUI_PSX_HAS_MENU_HOST 1
#endif
#endif

#if defined(RUI_PSX_HAS_MENU_HOST)
#include "psx_menu_host.h"
#include "recomp_launcher.h"
#include "recomp_ui_psx_menu.h"
#include "psx_menu_provider_internal.h"
#include "common/recomp_runtime_ui_internal.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace rui_psx {
namespace {

const PsxMenuHost *g_host = nullptr;
RecompRuntimeUi *g_ui = nullptr;
PsxHostVideoSettings g_video{};    // the menu's working copy, from video_get
unsigned g_restart = 0;            // restart-only edits made while open
unsigned g_restart_video = 0;      // ...from the video settings
unsigned g_restart_title = 0;      // ...from title rows (RECOMP_UI_PSX_TITLE_RESTART)
bool g_pause_in_menu = true;
bool g_paused = false;
int g_disc = 1;
char g_status[160];
char g_subtitle[96];
const RecompUiPsxMenuTitle *g_title = nullptr;  // recomp_ui_psx_menu_set_title
std::vector<RecompRuntimeUiItem> g_items;       // the extra rows of this attach

// Texture filtering offers the host's own vocabulary (the launcher game
// info's texture_filter catalog, shared with the pre-game launcher), so every
// frontend offers and keeps the same values. Without one: Nearest/Bilinear.
const char *const kLegacyFilters[] = {"Nearest", "Bilinear"};
const int kLegacyFilterValues[] = {0, 1};

RecompRuntimeUiItem g_extra[] = {
    {"graphics.texture_filter", "Graphics", "Texture filtering",
     "Nearest, bilinear, or xBR sampling of game textures.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 1, 1, kLegacyFilters, 2, kLegacyFilterValues},
    {"graphics.internal_resolution", "Graphics", "Internal resolution",
     "Rendering resolution; restarts when this menu closes.",
     RECOMP_RUNTIME_UI_CHOICE, 0, 0, 1, nullptr, 0, nullptr},
    {"graphics.render_thread", "Graphics", "Render thread",
     "Use a second CPU thread; restarts when this menu closes.",
     RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
    {"media.disc", "Disc", "Selected disc", "Choose a disc, then Insert disc and resume.",
     RECOMP_RUNTIME_UI_INT, 1, 1, 1, nullptr, 0, nullptr},
    {"media.insert", "Disc", "Insert disc and resume", "Change the disc while the game keeps running.",
     RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, nullptr, 0, nullptr},
};

enum { kTexFilter, kInternalRes, kRenderThread, kDisc, kInsert };

bool is(const RecompRuntimeUiItem *item, const char *key) { return item && item->key && !std::strcmp(item->key, key); }
bool is_title(const RecompRuntimeUiItem *item) {
    if (!g_title || !item || !g_title->items) return false;
    for (size_t i = 0; i < g_title->item_count; ++i)
        if (g_title->items[i].key && is(item, g_title->items[i].key)) return true;
    return false;
}
int disc_count() { return g_host && g_host->disc_count ? g_host->disc_count() : 0; }
bool netplay() { return g_host && g_host->netplay_active && g_host->netplay_active(); }

void status(const char *text) {
    std::snprintf(g_status, sizeof g_status, "%s", text);
    recomp_runtime_ui_set_status(g_ui, g_status);
}

void apply_video() {
    if (!g_host || !g_host->video_apply) return;
    const unsigned bits = g_host->video_apply(&g_video);
    if (bits & PSX_MENU_RESTART_AFTER_MATCH) {
        status("Applies after the match");
        return;  // never relaunch during netplay
    }
    g_restart_video = bits & (PSX_VIDEO_RESTART_RENDERER | PSX_VIDEO_RESTART_RESOLUTION | PSX_VIDEO_RESTART_THREADS);
    g_restart = g_restart_video | g_restart_title;
    if (g_restart) status("Applies with a quick restart when you close the menu");
}

int cb_get(void *, const RecompRuntimeUiItem *item, int *out) {
    if (is(item, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) *out = g_video.fullscreen;
    else if (is(item, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER)) *out = g_video.present_linear;
    else if (is(item, RECOMP_RUNTIME_UI_KEY_FMV_FILTER)) *out = g_video.fmv_filter;  // config value == choice index
    else if (is(item, "graphics.texture_filter")) *out = g_video.texture_filter;
    else if (is(item, "graphics.internal_resolution")) *out = g_video.internal_resolution;
    else if (is(item, "graphics.render_thread")) *out = g_video.render_thread;
    else if (is_title(item)) return g_title->get_value ? g_title->get_value(g_title->context, g_host, item, out) : 0;
    else if (is(item, RECOMP_RUNTIME_UI_KEY_PAUSE_IN_MENU)) *out = g_pause_in_menu ? 1 : 0;
    else if (is(item, "media.disc")) *out = g_disc;
    else return 0;
    return 1;
}

int cb_set(void *, const RecompRuntimeUiItem *item, int v) {
    if (is(item, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) g_video.fullscreen = v;
    else if (is(item, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER)) g_video.present_linear = v ? 1 : 0;
    else if (is(item, RECOMP_RUNTIME_UI_KEY_FMV_FILTER)) g_video.fmv_filter = v;
    else if (is(item, "graphics.texture_filter")) g_video.texture_filter = v;
    else if (is(item, "graphics.internal_resolution")) g_video.internal_resolution = v;
    else if (is(item, "graphics.render_thread")) g_video.render_thread = v ? 1 : 0;
    else if (is_title(item)) {
        const int r = g_title->set_value ? g_title->set_value(g_title->context, g_host, item, v) : 0;
        if (r & RECOMP_UI_PSX_TITLE_RESTART) {
            g_restart_title = PSX_VIDEO_RESTART_RENDERER;  // any bit: relaunch on close
            g_restart = g_restart_video | g_restart_title;
            status("Applies with a quick restart when you close the menu");
        }
        return r != 0;
    } else if (is(item, RECOMP_RUNTIME_UI_KEY_PAUSE_IN_MENU)) {
        g_pause_in_menu = v != 0;
        const bool want = g_pause_in_menu && recomp_runtime_ui_is_open(g_ui);
        if (g_host && g_host->set_paused) {
            if (want && !g_paused) g_paused = g_host->set_paused(1) != 0;
            else if (!want && g_paused) { g_host->set_paused(0); g_paused = false; }
        }
        return 1;
    } else if (is(item, "media.disc")) {
        if (v < 1 || v > disc_count()) return 0;
        g_disc = v;
        return 1;
    } else {
        return 0;
    }
    apply_video();
    return 1;
}

void close_menu() { recomp_runtime_ui_close(g_ui); }

int cb_action(void *, const RecompRuntimeUiItem *item) {
    if (!g_host) return 0;
    if (is(item, RECOMP_RUNTIME_UI_KEY_RESUME)) {
        close_menu();
    } else if (is(item, RECOMP_RUNTIME_UI_KEY_RETURN_TO_LAUNCHER)) {
        close_menu();
        g_host->request_exit(PSX_MENU_EXIT_TO_FRONTEND);
    } else if (is(item, RECOMP_RUNTIME_UI_KEY_QUIT)) {
        close_menu();
        g_host->request_exit(PSX_MENU_EXIT_QUIT);
    } else if (is(item, "media.insert")) {
        const int rc = g_host->disc_insert ? g_host->disc_insert(g_disc) : -1;
        if (rc != 0) {
            status(rc == 3 ? "Disc changes are unavailable in this game mode." : "That disc could not be inserted.");
            return 0;
        }
        close_menu();  // the lid cycle runs in the game
    }
    return 0;  // keep our own status instead of the generic "Done"
}

int cb_enabled(void *, const RecompRuntimeUiItem *item) {
    if (item && item->key && !std::strncmp(item->key, "media.", 6)) return disc_count() > 1 && !netplay();
    if (is_title(item) && g_title->is_enabled) return g_title->is_enabled(g_title->context, g_host, item);
    return 1;
}

void cb_visibility(void *, int open) {
    if (!g_host) return;
    if (open) {
        // Every opening starts at the first section, so the menu reads the
        // same way each time (and key-driven checks are deterministic).
        recomp_runtime_ui_enter_section(g_ui, 0);
        recomp_runtime_ui_leave_section(g_ui);
        if (g_host->video_get) g_host->video_get(&g_video);
        g_restart = g_restart_video = g_restart_title = 0;
        if (g_title && g_title->visibility_changed) g_title->visibility_changed(g_title->context, g_host, 1);
        const int mounted = g_host->disc_mounted ? g_host->disc_mounted() : 0;
        g_disc = mounted > 0 ? mounted : 1;
        if (disc_count() > 1) std::snprintf(g_subtitle, sizeof g_subtitle, "Mounted: Disc %d", mounted);
        else g_subtitle[0] = 0;
        g_paused = false;
        if (g_pause_in_menu && g_host->set_paused) {
            g_paused = g_host->set_paused(1) != 0;
            if (!g_paused) status("Online: the game keeps running");
        }
    } else {
        if (g_title && g_title->visibility_changed) g_title->visibility_changed(g_title->context, g_host, 0);
        if (g_paused && g_host->set_paused) g_host->set_paused(0);
        g_paused = false;
        if (g_restart) {
            g_restart = 0;
            g_host->request_exit(PSX_MENU_EXIT_RELAUNCH);
        }
    }
}

int p_attach(void *, void *, void *, const PsxMenuHost *host) {
    std::lock_guard<std::mutex> lk(lock());
    if (!host || host->abi_version != PSX_MENU_ABI_VERSION) return 0;
    // Chosen by default (the title chose no provider): keep the title's
    // presentation -- recomp-ui's game menu is for multi-disc titles.
#if defined(PSX_MENU_HOST_HAS_DEFAULT_SELECTION)
    // A title that registered its own rows chose this menu.
    if (!g_title && host->default_selection && (!host->disc_count || host->disc_count() < 2)) return 0;
#endif
    g_host = host;
    if (g_ui) recomp_runtime_ui_destroy(g_ui);
    const int discs = disc_count();
    g_extra[kDisc].maximum = discs > 1 ? discs : 1;
    // The host's texture-filter catalog (attach runs on the main thread, where
    // launcher_game_info may be called).
    g_extra[0].choices = kLegacyFilters;
    g_extra[0].choice_values = kLegacyFilterValues;
    g_extra[0].choice_count = 2;
#if defined(RECOMP_LAUNCHER_HAS_TEXTURE_FILTER_CATALOG)
    if (host->launcher_game_info) {
        RecompLauncherCGameInfo gi{};
        host->launcher_game_info(&gi);
        if (gi.texture_filter_labels && gi.texture_filter_values && gi.num_texture_filters > 0) {
            g_extra[0].choices = gi.texture_filter_labels;
            g_extra[0].choice_values = gi.texture_filter_values;
            g_extra[0].choice_count = (size_t)gi.num_texture_filters;
        }
    }
#endif
    g_extra[0].minimum = 0;
    g_extra[0].maximum = (int)g_extra[0].choice_count - 1;
    // Internal resolution: only with the host's catalog (same vocabulary and
    // encoding as the pre-game launcher); restart-only through video_apply.
    bool have_ir = false;
#if defined(RECOMP_LAUNCHER_HAS_INTERNAL_RESOLUTION)
    if (host->launcher_game_info) {
        RecompLauncherCGameInfo gi{};
        host->launcher_game_info(&gi);
        if (gi.internal_resolution_labels && gi.internal_resolution_values && gi.num_internal_resolutions > 0) {
            g_extra[kInternalRes].choices = gi.internal_resolution_labels;
            g_extra[kInternalRes].choice_values = gi.internal_resolution_values;
            g_extra[kInternalRes].choice_count = (size_t)gi.num_internal_resolutions;
            g_extra[kInternalRes].maximum = gi.num_internal_resolutions - 1;
            have_ir = true;
        }
    }
#endif
    g_items.clear();
    g_items.push_back(g_extra[kTexFilter]);
    if (have_ir) g_items.push_back(g_extra[kInternalRes]);
    g_items.push_back(g_extra[kRenderThread]);
    if (discs > 1) { g_items.push_back(g_extra[kDisc]); g_items.push_back(g_extra[kInsert]); }
    if (g_title && g_title->items)
        g_items.insert(g_items.end(), g_title->items, g_title->items + g_title->item_count);
    RecompRuntimeUiStandardConfig cfg{};
    cfg.menu.title = g_title && g_title->menu_title ? g_title->menu_title : "Game menu";
    cfg.menu.subtitle = g_subtitle;
    cfg.menu.theme = "psx";
    cfg.menu.accept_label = "Enter / Cross";
    cfg.menu.back_label = "Esc / Circle";
    cfg.menu.callbacks.get_value = cb_get;
    cfg.menu.callbacks.set_value = cb_set;
    cfg.menu.callbacks.run_action = cb_action;
    cfg.menu.callbacks.is_enabled = cb_enabled;
    cfg.menu.callbacks.visibility_changed = cb_visibility;
    cfg.features = RECOMP_RUNTIME_UI_STANDARD_FULLSCREEN | RECOMP_RUNTIME_UI_STANDARD_LINEAR_FILTER |
                   RECOMP_RUNTIME_UI_STANDARD_FMV_FILTER | RECOMP_RUNTIME_UI_STANDARD_PAUSE_IN_MENU |
                   RECOMP_RUNTIME_UI_STANDARD_RESUME | RECOMP_RUNTIME_UI_STANDARD_RETURN_TO_LAUNCHER |
                   RECOMP_RUNTIME_UI_STANDARD_QUIT;
    cfg.extra_items = g_items.data();
    cfg.extra_item_count = g_items.size();
    g_ui = recomp_runtime_ui_create_standard(&cfg);
    if (!g_ui) { g_host = nullptr; return 0; }
    return 1;
}

int p_event(void *, const void *ev) { return sdl_event(ev); }
int p_is_open(void *) {
    std::lock_guard<std::mutex> lk(lock());
    return g_ui && recomp_runtime_ui_is_open(g_ui);
}
void p_set_open(void *, int open) {
    std::lock_guard<std::mutex> lk(lock());
    if (!g_ui) return;
    if (open) recomp_runtime_ui_open(g_ui);
    else recomp_runtime_ui_close(g_ui);
}
void p_draw(void *, int w, int h) { draw(w, h); }
void p_detach(void *) {
    release_gl();
    std::lock_guard<std::mutex> lk(lock());
    if (g_ui) {
        recomp_runtime_ui_close(g_ui);   // drops a held pause
        recomp_runtime_ui_destroy(g_ui);
    }
    g_ui = nullptr;
    g_host = nullptr;
}

const PsxMenuProvider g_provider = {
    PSX_MENU_ABI_VERSION, "recomp-ui", nullptr,
    p_attach, p_event, p_is_open, p_set_open, p_draw, p_detach,
};

}  // namespace

std::mutex &lock() {
    static std::mutex m;
    return m;
}
RecompRuntimeUi *model() { return g_ui; }
void input(int in) {
    if (!g_ui) return;
    if (in == RECOMP_RUNTIME_UI_INPUT_TOGGLE) {
        if (recomp_runtime_ui_is_open(g_ui)) recomp_runtime_ui_close(g_ui);
        else recomp_runtime_ui_open(g_ui);
        return;
    }
    recomp_runtime_ui_handle_input(g_ui, (RecompRuntimeUiInput)in, 1, 0);
}

}  // namespace rui_psx

extern "C" const PsxMenuProvider *recomp_ui_psx_menu_provider(void) { return &rui_psx::g_provider; }
extern "C" void recomp_ui_psx_menu_set_title(const RecompUiPsxMenuTitle *title) {
    std::lock_guard<std::mutex> lk(rui_psx::lock());
    rui_psx::g_title = title;
}
extern "C" void recomp_ui_psx_menu_input(int in) {
    std::lock_guard<std::mutex> lk(rui_psx::lock());
    rui_psx::input(in);
}

#endif  // RUI_PSX_HAS_MENU_HOST
