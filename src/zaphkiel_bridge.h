// zaphkiel_bridge.h
// Drop this into your C++ project and include it in main.cpp.
// No .cpp file needed — everything is resolved at runtime via dlsym
// since Zaphkiel is injected as a .so into the same process.

#pragma once
#include <dlfcn.h>

// ── declarations (implemented in Rust, resolved at runtime) ──────────────────

// Call from drawmenu() inside your tab 3 block:
//
//   } else if (current_tab == 3) {
//       Zaphkiel::DrawTab();
//   }
//
// Call from OnTouchCallback:
//
//   Zaphkiel::NotifyTouch(action, x, y);

namespace Zaphkiel {

    inline void DrawTab() {
        static void (*fn)() = nullptr;
        if (!fn) fn = (void(*)())dlsym(RTLD_DEFAULT, "zaphkiel_draw_tab");
        if (fn) fn();
    }

    inline bool NotifyTouch(int action, float x, float y) {
        static bool (*fn)(int, float, float) = nullptr;
        if (!fn) fn = (bool(*)(int,float,float))dlsym(RTLD_DEFAULT, "zaphkiel_notify_touch");
        return fn ? fn(action, x, y) : false;
    }

} // namespace Zaphkiel
