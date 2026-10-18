/* recomp_ui_psx_menu.h -- recomp-ui as a psxrecomp in-game menu provider.
 *
 * psxrecomp owns the provider contract (runtime/include/psx_menu_host.h):
 * the runtime keeps every policy (pause, live settings, mod changes,
 * disc/tray, netplay session start and exit) and offers it to any provider
 * through PsxMenuHost. recomp-ui implements PsxMenuProvider with its shared
 * runtime-UI model (recomp_runtime_ui.h) and ImGui presentation:
 *
 *   - Display: fullscreen; Graphics: texture filtering (Nearest, Bilinear,
 *     xBR -- the runtime's own enum), FMV filtering, linear filter, render
 *     thread (restart-only: applied by a quick relaunch when the menu
 *     closes); System: pause in menu, resume, return to launcher, quit;
 *     Disc: selected disc and Insert disc and resume (multi-disc titles,
 *     through the runtime's tray policy).
 *   - Opens with F10 or Ctrl+L, or Select + L3 on a controller; arrows,
 *     Enter/Space, Esc/Backspace, D-pad, Cross/Circle navigate.
 *
 * psxrecomp uses this provider when the title registers none (recomp-ui is
 * the default). Chosen that way (PsxMenuHost.default_selection) it keeps the
 * title's default presentation: it attaches only for multi-disc titles,
 * which is where recomp-ui's game menu appears today, unless the title
 * registers its own rows (recomp_ui_psx_menu_set_title below).
 *
 * Built only inside a psxrecomp title build (the runtime's include path has
 * psx_menu_host.h); declared here so the runtime can find it with
 * __has_include. */
#ifndef RECOMP_UI_PSX_MENU_H
#define RECOMP_UI_PSX_MENU_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PsxMenuProvider;
const struct PsxMenuProvider *recomp_ui_psx_menu_provider(void);

/* For hosts and tests without SDL events: one RecompRuntimeUiInput press
 * (RECOMP_RUNTIME_UI_INPUT_TOGGLE opens/closes). Main thread. */
void recomp_ui_psx_menu_input(int recomp_runtime_ui_input);

/* Title rows: a title adds its own settings (mod features, game options) to
 * recomp-ui's menu instead of writing a provider of its own. Registering one
 * is also the title choosing this menu, so it attaches for single-disc titles
 * too. Items are appended after the standard rows; their strings and the
 * struct must outlive the menu. Callbacks run under the menu's lock and get
 * the runtime's host (psx_menu_host.h) for mod_set and friends.
 *
 * set_value returns RECOMP_UI_PSX_TITLE_* bits; 0 refuses the value. */
#define RECOMP_UI_PSX_MENU_HAS_TITLE_ITEMS 1
enum {
    RECOMP_UI_PSX_TITLE_APPLIED = 1,
    /* Takes effect after a quick relaunch when the menu closes. */
    RECOMP_UI_PSX_TITLE_RESTART = 2
};
struct PsxMenuHost;
struct RecompRuntimeUiItem;
typedef struct RecompUiPsxMenuTitle {
    const char *menu_title;          /* NULL: "Game menu" */
    const struct RecompRuntimeUiItem *items;
    size_t item_count;
    void *context;
    int (*get_value)(void *context, const struct PsxMenuHost *host,
                     const struct RecompRuntimeUiItem *item, int *value_out);
    int (*set_value)(void *context, const struct PsxMenuHost *host,
                     const struct RecompRuntimeUiItem *item, int value);
    /* Optional: NULL means always enabled. */
    int (*is_enabled)(void *context, const struct PsxMenuHost *host,
                      const struct RecompRuntimeUiItem *item);
    /* Optional: the menu opened (1) or closed (0). */
    void (*visibility_changed)(void *context, const struct PsxMenuHost *host, int open);
} RecompUiPsxMenuTitle;
/* Main thread, before the game window opens (or NULL to clear). */
void recomp_ui_psx_menu_set_title(const RecompUiPsxMenuTitle *title);

#ifdef __cplusplus
}
#endif
#endif
