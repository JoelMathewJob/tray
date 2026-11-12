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

    // 1. Set Immersive Dark Mode for DWM
    BOOL darkMode = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

    // 2. Enable native Windows 11 hardware-accelerated rounded corners
    DWORD cornerPref = 2; // DWMWCP_ROUND (native GPU rounding for window bounds, blur, and border)
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

    // 3. Set subtle 1px hardware frame color (eliminates thick white borders)
    COLORREF borderColor;
    if (isTinted) {
        borderColor = isDark ? RGB(55, 60, 75) : RGB(190, 198, 212);
    } else {
        borderColor = isDark ? RGB(65, 72, 90) : RGB(200, 208, 224);
    }
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &borderColor, sizeof(borderColor));

    // 4. Extend frame into client area for DWM glass composition
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    auto pSetWindowCompositionAttribute = hUser ? 
        (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute") : nullptr;

    if (isTinted) {
        // Tinted mode: disable system backdrop, apply Acrylic with frosted color
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
        // Liquid Glass mode: Use Windows 11 native Acrylic system backdrop (DWMSBT_ACRYLIC = 3)
        // First, clear any old accent policy
        if (pSetWindowCompositionAttribute) {
            ACCENT_POLICY policy = { ACCENT_DISABLED, 0, 0, 0 };
            WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
            pSetWindowCompositionAttribute(hwnd, &data);
        }

        // Apply native Windows 11 Acrylic Glass
        DWORD backdropType = 3; // DWMSBT_ACRYLIC (true Windows 11 glassmorphism)
        HRESULT hr = DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdropType, sizeof(backdropType));

        if (FAILED(hr) && pSetWindowCompositionAttribute) {
            // Fallback for Windows 10
            DWORD tintColor = isDark ? 0x2A18141C : 0x20F6F3EE;
            ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 2, tintColor, 0 };
            WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
            pSetWindowCompositionAttribute(hwnd, &data);
        }
    }
}
