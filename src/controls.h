#pragma once
#include <windows.h>
#include <string>

enum PowerMode
{
    POWER_MODE_BATTERY = 0,
    POWER_MODE_BALANCED = 1,
    POWER_MODE_PERFORMANCE = 2
};

struct QuickControlsState
{
    float brightness = 1.0f; // 0.0f to 1.0f
    float volume = 1.0f;     // 0.0f to 1.0f
    bool isMuted = false;

    bool wifiOn = true;
    bool bluetoothOn = true;
    bool nightLightOn = false;
    bool focusOn = false;

    int batteryPercent = 100;
    bool isPluggedIn = true;
    bool hasBattery = true;

    PowerMode powerMode = POWER_MODE_PERFORMANCE;
};

// Hit rects and interaction state
extern RECT g_tabFilesRect;
extern RECT g_tabControlsRect;
extern RECT g_settingsBtnRect;
extern RECT g_brightnessCapsuleRect;
extern RECT g_volumeCapsuleRect;
extern RECT g_tileRects[6];
extern RECT g_wifiArrowRect;
extern RECT g_btArrowRect;
extern RECT g_powerModeRects[3];

extern bool g_tabFilesHovered;
extern bool g_tabControlsHovered;
extern bool g_settingsHovered;
extern bool g_wifiArrowHovered;
extern bool g_btArrowHovered;
extern bool g_draggingBrightness;
extern bool g_draggingVolume;
extern int g_hoveredTile;
extern int g_hoveredPowerMode;

void InitQuickControls();
void ShutdownQuickControls();
void UpdateQuickControlsState();
QuickControlsState &GetQuickControlsState();

void SetSystemVolume(float level);
void ToggleSystemMute();
void SetSystemBrightness(float level);
void ToggleWifi();
void ToggleBluetooth();
void ToggleNightLight();
void ToggleFocus();
void SetPowerMode(PowerMode mode);

void RenderTopNavBar(HDC hdc, int width, int height, bool isDark, int activeTab, bool settingsHovered, int dockEdge);
void RenderQuickControls(HDC hdc, int width, int height, bool isDark, int dockEdge);
bool HandleQuickControlsClick(HWND hwnd, POINT pt);
bool HandleQuickControlsMouseMove(HWND hwnd, POINT pt);
void HandleQuickControlsMouseUp(HWND hwnd);
bool HandleQuickControlsWheel(HWND hwnd, POINT pt, short delta);
