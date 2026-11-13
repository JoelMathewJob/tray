#pragma once
#include <windows.h>
#include <string>

struct ScreenRecordingState
{
    bool isRecording = false;
    ULONGLONG startTime = 0;
    std::wstring outputPath = L"";
    HANDLE hProcess = nullptr;
    HANDLE hStdinWrite = nullptr;
};

struct ActionToast
{
    bool active = false;
    std::wstring message = L"";
    std::wstring iconGlyph = L"\xE73E";
    COLORREF iconColor = RGB(34, 197, 94);
    ULONGLONG showTime = 0;
};

extern ScreenRecordingState g_recording;
extern ActionToast g_toast;

// Row 1: The 3 Core Action Buttons
extern RECT g_actionRecBtnRect;
extern RECT g_actionSnipBtnRect;
extern RECT g_actionShotBtnRect;

// Row 2: Utility Action Buttons
extern RECT g_actionFolderBtnRect;
extern RECT g_actionClipboardBtnRect;
extern RECT g_actionLockBtnRect;

// Hover States
extern bool g_actionRecHovered;
extern bool g_actionSnipHovered;
extern bool g_actionShotHovered;
extern bool g_actionFolderHovered;
extern bool g_actionClipboardHovered;
extern bool g_actionLockHovered;

void InitActionsEngine();
void ShutdownActionsEngine();

// Notification Toast Helpers
void ShowActionToast(const std::wstring &msg, const wchar_t *glyph = L"\xE73E", COLORREF iconCol = RGB(34, 197, 94));
void RenderActionToast(HDC hdc, int width, int height, bool isDark);

// Screen Recording Functions
bool StartScreenRecording(HWND hwnd);
bool StopScreenRecording(HWND hwnd);
void ToggleScreenRecording(HWND hwnd);
std::wstring GetRecordingDurationString();

// Screenshot Functions
bool CaptureInstantScreenshot(HWND hwnd);
bool LaunchRegionSnippingTool();

// Audio Peak Measurement (WASAPI Master Peak)
float GetSystemAudioPeak();

// Collapsed Dock Content Renderer (Recording HUD / Live Clock / Dancing Visualizer / Minimalist Pill)
void RenderCollapsedDockContent(HDC hdc, int width, int height, bool isDark, bool isVert);

// Actions Tab Content Renderer & Event Handlers
void RenderActionsTab(HDC hdc, int width, int height, bool isDark, int dockEdge);
bool HandleActionsTabClick(HWND hwnd, POINT pt);
bool HandleActionsTabMouseMove(HWND hwnd, POINT pt);
int GetActionsTabDesiredWidth(int dockEdge);
int GetActionsTabDesiredHeight(int dockEdge);
