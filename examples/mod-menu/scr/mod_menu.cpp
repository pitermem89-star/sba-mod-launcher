// mod-menu: adds a "Mods" button to the pause menu and a window with the list
// of installed mods. Works like a normal mod: everything goes through the
// Bearite API, nothing here is part of the loader.
//
// How it works:
//   1. Hook PauseMenuManager.Start. After the original runs, clone the
//      "Achievements" button and the "About" window (both already exist in the
//      pause menu prefab), retitle the clone to "Mods".
//   2. Hook UnityEngine.UI.Button.OnPointerClick / OnSubmit. If the clicked
//      button is our clone, open our window instead of calling the original
//      handler (the clone still carries the Achievements listener from the
//      prefab, so the original must not run). Any button inside our window
//      closes it.
//   3. Mods that declare "settings" in their mod.json get a "Настроить" button;
//      the values are stored in <mod dir>/settings.json (see settings.inc).
//   4. IL2CPP calls (Instantiate, SetActive, set_text, ...) go through
//      il2cpp_runtime_invoke, resolved from libil2cpp.so with dlsym.
#include "bearite.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char* TAG = "mod-menu";

// Field offsets from dump.cs (PauseMenuManager : SbaBehaviour).
constexpr size_t OFF_ABOUT_WINDOW = 0x28;
constexpr size_t OFF_ACHIEVEMENTS_BUTTON = 0x98;
constexpr size_t OFF_CLOSE_ABOUT_BUTTON = 0xA0;


// The code is split into several files (each one is included right here, in
// order) so that every file stays small.
#include "il2cpp_glue.inc"   // dlsym'ed il2cpp API + Unity helpers
#include "mods_scan.inc"     // finds installed mods and reads mod.json
#include "ui_core.inc"       // state, UI building blocks (buttons, labels, ...)
#include "shapes.inc"        // procedurally drawn sprites (rounded rectangle, gear)
#include "settings.inc"      // per-mod settings: mod.json schema, settings.json, settings screen
#include "ui_views.inc"      // window contents, building the menu, actions
#include "hooks.inc"         // game hooks

}  // namespace

// ------------------------------------------------------- mod entry points --
extern "C" {

BEARITE_EXPORT int bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  bool a = bearite::hook(ASM_GAME, "", "PauseMenuManager", "Start", 0,
                         reinterpret_cast<void*>(&hk_start), reinterpret_cast<void**>(&orig_start));
  bool b = bearite::hook(ASM_UI, "UnityEngine.UI", "Button", "OnPointerClick", 1,
                         reinterpret_cast<void*>(&hk_click), reinterpret_cast<void**>(&orig_click));
  bool c = bearite::hook(ASM_UI, "UnityEngine.UI", "Button", "OnSubmit", 1,
                         reinterpret_cast<void*>(&hk_submit), reinterpret_cast<void**>(&orig_submit));
  bearite::log(BEARITE_LOG_INFO, TAG, "loaded, hooks: Start=%d OnPointerClick=%d OnSubmit=%d", a, b, c);
  return (a && b && c) ? 0 : 1;
}

BEARITE_EXPORT void bearite_on_unload(void) { g.btn = nullptr; }

}  // extern "C"
