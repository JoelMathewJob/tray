#pragma once
#include <windows.h>
#include <vector>
#include <string>

constexpr float COLLAPSED_W = 210.0f;
constexpr float COLLAPSED_H = 26.0f;
constexpr float COLLAPSED_LENGTH = 210.0f;
constexpr float COLLAPSED_THICKNESS = 26.0f;
constexpr int CORNER_RADIUS = 24;
constexpr UINT_PTR ANIM_TIMER_ID = 42;
constexpr int HOTKEY_TOGGLE_ID = 9001;
constexpr UINT WM_DOWNLOAD_COMPLETE = WM_USER + 105;

enum DockEdge
{
    EDGE_TOP = 0,
    EDGE_RIGHT = 1,
    EDGE_LEFT = 2
};

enum NotchStyle
{
    NOTCH_ATTACHED = 0, // Flush against screen bezel
    NOTCH_FLOATING = 1  // Detached floating dock
};

enum ActiveTab
{
    TAB_FILES = 0,
    TAB_CONTROLS = 1,
    TAB_ACTIONS = 2,
    TAB_MUSIC = 3
};

enum CollapsedStyle
{
    COLLAPSED_DEFAULT = 0, // Classic Grab Pill
    COLLAPSED_TIME = 1,    // Live Clock & Date
    COLLAPSED_AUDIO = 2,   // Live Equalizer / Audio Visualizer
    COLLAPSED_DYNAMIC = 3  // Dynamic (Recording > Audio > Time > Pill)
};

struct AppSettings
{
    bool moveFiles = false;
    bool autoRemoveUnpinned = true;
    bool enableMagnify = true;
    bool largeIcons = false;
    bool alwaysOnTop = true;
    int hotkeyMods = MOD_WIN | MOD_ALT;
    int hotkeyVk = 'T';
    int dockEdge = EDGE_TOP;
    float dockOffsetRatio = 0.5f;
    int themeMode = 0; // 0 = System, 1 = Light, 2 = Dark
    int backdropStyle = 0; // 0 = Liquid Glass (Acrylic), 1 = Tinted (Candy)
    int notchStyle = NOTCH_ATTACHED; // 0 = Attached Notch, 1 = Floating Dock
    int cornerRadius = 24; // Customizable dock roundness
    int collapsedStyle = COLLAPSED_DYNAMIC; // 0=Pill, 1=Time, 2=Audio, 3=Dynamic

    // Actions & Recording preferences
    bool recIncludeAudio = true;
    bool recAutoDropInTray = true;

    // Quick Controls customization
    bool qcShowWifi = true;
    bool qcShowBluetooth = true;
    bool qcShowAirplane = true;
    bool qcShowHotspot = true;
    bool qcShowNightLight = true;
    bool qcShowFocus = true;
    bool qcShowMute = true;
    bool qcShowBattery = true;
    bool qcShowBrightness = true;
    bool qcShowVolume = true;
    bool qcShowPowerMode = true;
    bool qcShowLockPC = true;

    // Player preferences
    bool showMusicTab = true;
    bool playerShowCollapsedArt = true;
    bool playerShowVisualizer = true;
    int playerVisualizerColor = 0; // 0=Coral, 1=Cyan, 2=Emerald, 3=Amber
};

struct ThemePalette {
    COLORREF border, textPrimary, textSec, pillHover, pillBorder;
    COLORREF selBg, selBorder, tipBg, div, scrollTrack, scrollThumb;
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
extern ThemePalette g_theme;
extern ActiveTab g_activeTab;

void CalculateTargetSize(bool expanded);
void PruneMissingItems(HWND hwnd);
void TriggerExpand(HWND hwnd);
int AddPlaceholderItem(const std::wstring& tempName);
void AddFileToTray(const std::wstring& filePath, bool isPinned = false, const std::wstring& customName = L"");
void ApplyGlobalHotkey(HWND hwnd);
void UpdateTrayWindowGeometry(HWND hwnd);
void SyncTrayOrientation(HWND hwnd);
void UpdateThemeColors();
bool IsResolvedDarkMode();

// Temp folder lifecycle helpers
std::wstring GetWinTrayTempDir();
void DeleteTempFileIfOwned(const std::wstring& filePath);
void CleanupOldTempFiles(int maxAgeDays = 3);