#pragma once
#include <windows.h>
#include <vector>
#include <string>

constexpr float COLLAPSED_W = 220.0f;
constexpr float COLLAPSED_H = 16.0f;
constexpr float COLLAPSED_LENGTH = 220.0f;
constexpr float COLLAPSED_THICKNESS = 16.0f;
constexpr int CORNER_RADIUS = 28;
constexpr UINT_PTR ANIM_TIMER_ID = 42;
constexpr int HOTKEY_TOGGLE_ID = 9001;
constexpr UINT WM_DOWNLOAD_COMPLETE = WM_USER + 105;

enum DockEdge
{
    EDGE_TOP = 0,
    EDGE_RIGHT = 1,
    EDGE_LEFT = 2
};

struct AppSettings
{
    bool moveFiles = false;
    bool autoRemoveUnpinned = true;
    bool enableMagnify = true;
    bool largeIcons = false;
    int hotkeyMods = MOD_WIN | MOD_ALT;
    int hotkeyVk = 'T';
    int dockEdge = EDGE_TOP;
    float dockOffsetRatio = 0.5f;
};

struct PinnedItem
{
    int id = 0;
    std::wstring path;
    std::wstring name;
    HICON icon = nullptr;
    RECT rect = {};
    bool isSelected = false;
    bool isPinned = false;
    bool isLoading = false;
    float currentScale = 1.0f;
};

struct DownloadResult
{
    int itemId;
    bool success;
    std::wstring finalPath;
    std::wstring displayName;
};

extern HWND g_mainHwnd;
extern AppSettings g_settings;
extern std::vector<PinnedItem> g_items;
extern bool g_isExpanded;
extern bool g_isSelfDragging;
extern float g_SpinnerAngle;

void CalculateTargetSize(bool expanded);
void PruneMissingItems(HWND hwnd);
void TriggerExpand(HWND hwnd);
int AddPlaceholderItem(const std::wstring& tempName);
void AddFileToTray(const std::wstring& filePath, bool isPinned = false, const std::wstring& customName = L"");
void ApplyGlobalHotkey(HWND hwnd);
void UpdateTrayWindowGeometry(HWND hwnd);

// Temp folder lifecycle helpers
std::wstring GetWinTrayTempDir();
void DeleteTempFileIfOwned(const std::wstring& filePath);
void CleanupOldTempFiles(int maxAgeDays = 3);