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
typedef struct _ACCENT_POLICY { ACCENT_STATE AccentState; DWORD AccentFlags; DWORD GradientColor; DWORD AnimationId; } ACCENT_POLICY;
typedef struct _WINDOWCOMPOSITIONATTRIBDATA { WINDOWCOMPOSITIONATTRIB Attrib; PVOID pvData; SIZE_T cbData; } WINDOWCOMPOSITIONATTRIBDATA;
typedef BOOL(WINAPI* pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

void ApplyWindowBackdrop(HWND hwnd, BackdropType type) {
    bool isTinted = (g_settings.backdropStyle == 1);
    bool isDark = IsResolvedDarkMode();

    // Set immersive dark mode attribute
    BOOL darkMode = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

    if (isTinted) {
        // Tinted mode: Disable DWM system backdrop, use SetWindowCompositionAttribute
        // with a translucent gradient color for a solid tinted look
        DWORD backdropNone = 1; // DWMSBT_NONE - disable system backdrop
        DwmSetWindowAttribute(hwnd, 38, &backdropNone, sizeof(backdropNone));

        // Extend frame for glass effect
        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);

        HMODULE hUser = GetModuleHandleW(L"user32.dll");
        if (hUser) {
            auto SetWindowCompositionAttribute = (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
            if (SetWindowCompositionAttribute) {
                // ABGR format: Alpha | Blue | Green | Red
                // Dark: deep dark translucent,  Light: frosted white translucent
                DWORD tintColor = isDark ? 0xCC1A1A2E : 0xBBF0F0F5;
                ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 2, tintColor, 0 };
                WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
                SetWindowCompositionAttribute(hwnd, &data);
            }
        }
    } else {
        // Glass mode: Use DWM system acrylic backdrop (clean glass look)
        // First, clear any old composition attribute by disabling it
        HMODULE hUser = GetModuleHandleW(L"user32.dll");
        if (hUser) {
            auto SetWindowCompositionAttribute = (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
            if (SetWindowCompositionAttribute) {
                ACCENT_POLICY policy = { ACCENT_DISABLED, 0, 0, 0 };
                WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
                SetWindowCompositionAttribute(hwnd, &data);
            }
        }

        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);

        DWORD backdropType = 3; // DWMSBT_ACRYLIC
        HRESULT hr = DwmSetWindowAttribute(hwnd, 38, &backdropType, sizeof(backdropType));

        if (FAILED(hr)) {
            // Fallback for older Windows 10 builds
            if (hUser) {
                auto SetWindowCompositionAttribute = (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
                if (SetWindowCompositionAttribute) {
                    ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 2, 0x00000000, 0 };
                    WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
                    SetWindowCompositionAttribute(hwnd, &data);
                }
            }
        }
    }
}
