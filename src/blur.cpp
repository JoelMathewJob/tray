#include "blur.h"
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

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

    // Enforce native Windows 11 hardware-accelerated rounded corners
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        auto SetWindowCompositionAttribute = 
            (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
        
        if (SetWindowCompositionAttribute) {
            ACCENT_POLICY policy = {};
            policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
            policy.AccentFlags = 2;
            
            // Your original dark glass tint
            policy.GradientColor = 0x40121214;

            WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
            SetWindowCompositionAttribute(hwnd, &data);
        }
    }
}