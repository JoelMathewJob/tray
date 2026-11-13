#include "blur.h"
#include "state.h"
#include <dwmapi.h>

#pragma comment(lib, "Dwmapi.lib")

typedef enum _WINDOWCOMPOSITIONATTRIB { WCA_ACCENT_POLICY = 19 } WINDOWCOMPOSITIONATTRIB;
typedef enum _ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
} ACCENT_STATE;
typedef struct _ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
} ACCENT_POLICY;
typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attrib;
    PVOID pvData;
    SIZE_T cbData;
} WINDOWCOMPOSITIONATTRIBDATA;
typedef BOOL(WINAPI* pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

void ApplyWindowBackdrop(HWND hwnd, BackdropType type) {
    if (!hwnd || !IsWindow(hwnd)) return;

    bool isTinted = (g_settings.backdropStyle == 1);
    bool isDark   = IsResolvedDarkMode();

    // 1. Completely disable DWM drop shadows
    int ncPolicy = 1; // DWMNCRP_DISABLED - removes window drop shadow
    DwmSetWindowAttribute(hwnd, 2 /* DWMWA_NCRENDERING_POLICY */, &ncPolicy, sizeof(ncPolicy));

    // 2. Set Immersive Dark Mode for DWM
    BOOL darkMode = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

    // 3. Enable native Windows 11 hardware-accelerated rounded corners
    DWORD cornerPref = 2; // DWMWCP_ROUND (native GPU rounding for window bounds and blur)
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

    // 4. Remove DWM border completely (removes black/white outer border!)
    COLORREF borderColor = 0xFFFFFFFE; // DWMWA_COLOR_NONE - completely disables outer frame
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &borderColor, sizeof(borderColor));

    // 5. Extend frame into client area for DWM glass composition
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    auto pSetWindowCompositionAttribute = hUser ? 
        (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute") : nullptr;

    if (isTinted) {
        // Tinted mode: frosted candy glass (~50% opacity)
        DWORD backdropNone = 1; // DWMSBT_NONE
        DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdropNone, sizeof(backdropNone));

        if (pSetWindowCompositionAttribute) {
            // Format: 0xAABBGGRR
            DWORD tintColor = isDark ? 0x85241C22 : 0x75F4ECE6;
            ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 2, tintColor, 0 };
            WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
            pSetWindowCompositionAttribute(hwnd, &data);
        }
    } else {
        // Liquid Glass mode:
        // In Light Mode, Windows 11 DWMSBT_ACRYLIC injects an opaque 95% blinding white sheet.
        // We bypass that with DWMSBT_NONE + ACCENT_ENABLE_ACRYLICBLURBEHIND (0x28F0ECE8)
        // so Light Mode is true translucent glassmorphism with wallpaper refraction!
        if (!isDark) {
            DWORD backdropNone = 1; // DWMSBT_NONE
            DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdropNone, sizeof(backdropNone));

            if (pSetWindowCompositionAttribute) {
                DWORD tintColor = 0x28F0ECE8; // Translucent frosted glass (15% tint, 85% wallpaper refraction)
                ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 2, tintColor, 0 };
                WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
                pSetWindowCompositionAttribute(hwnd, &data);
            }
        } else {
            // Dark Mode: true dark crystal glass
            if (pSetWindowCompositionAttribute) {
                DWORD tintColor = 0x2A18141C;
                ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 2, tintColor, 0 };
                WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
                pSetWindowCompositionAttribute(hwnd, &data);
            }

            DWORD backdropType = 3; // DWMSBT_ACRYLIC
            DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdropType, sizeof(backdropType));
        }
    }
}
