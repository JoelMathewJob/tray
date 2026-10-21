#include "blur.h"
#include "state.h" // Needed to check current theme
#include <dwmapi.h>

#pragma comment(lib, "Dwmapi.lib")

typedef enum _WINDOWCOMPOSITIONATTRIB { WCA_ACCENT_POLICY = 19 } WINDOWCOMPOSITIONATTRIB;
typedef enum _ACCENT_STATE { ACCENT_ENABLE_ACRYLICBLURBEHIND = 4 } ACCENT_STATE;
typedef struct _ACCENT_POLICY { ACCENT_STATE AccentState; DWORD AccentFlags; DWORD GradientColor; DWORD AnimationId; } ACCENT_POLICY;
typedef struct _WINDOWCOMPOSITIONATTRIBDATA { WINDOWCOMPOSITIONATTRIB Attrib; PVOID pvData; SIZE_T cbData; } WINDOWCOMPOSITIONATTRIBDATA;
typedef BOOL(WINAPI* pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

void ApplyWindowBackdrop(HWND hwnd, BackdropType type) {
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Dynamically apply Dark or Light immersive glass
    BOOL darkMode = IsResolvedDarkMode() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

    DWORD backdropType = 3; // DWMSBT_ACRYLIC
    HRESULT hr = DwmSetWindowAttribute(hwnd, 38, &backdropType, sizeof(backdropType));

    if (FAILED(hr)) {
        HMODULE hUser = GetModuleHandleW(L"user32.dll");
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