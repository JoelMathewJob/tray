#include "settings.h"
#include "state.h"
#include "controls.h"
#include "actions.h"
#include "player.h"
#include "updater.h"
#include "blur.h"
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <dwmapi.h>

#pragma comment(lib, "Dwmapi.lib")

static std::wstring g_iniPath;

enum SettingsTab
{
    STAB_GENERAL = 0,
    STAB_APPEARANCE = 1,
    STAB_PLAYER = 2,
    STAB_ACTIONS = 3,
    STAB_TRAY = 4,
    STAB_CONTROLS = 5,
    STAB_ABOUT = 6,
    STAB_COUNT = 7
};

static SettingsTab s_activeTab = STAB_APPEARANCE;
static int s_hoveredTab = -1;
static bool s_closeHovered = false;
static bool s_previewOpen = false;

// Custom Search State (Native GDI, No white edit box, No broken glyphs)
static std::wstring s_searchQuery = L"";
static bool s_searchFocused = false;
static UINT_PTR s_caretTimerId = 101;
static bool s_caretVisible = true;
static RECT s_searchBoxRect = {};
static RECT s_searchClearRect = {};

// Hotkey Recording State
static bool s_isRecordingHotkey = false;
static RECT s_hotkeyBtnRect = {};

// Hit test rectangles
static RECT s_tabRects[STAB_COUNT] = {};
static RECT s_closeBtnRect = {};

// Appearance Tab hit rects
static RECT s_previewBoxRect = {};
static RECT s_previewToggleRect = {};
static RECT s_themePillRects[3] = {};
static RECT s_backdropPillRects[2] = {};
static RECT s_notchPillRects[2] = {};
static RECT s_collapsedPillRects[4] = {};
static RECT s_largeIconsToggleRect = {};
static RECT s_magnifyToggleRect = {};

// Player Tab hit rects
static RECT s_playerShowMusicTabToggleRect = {};
static RECT s_playerShowArtToggleRect = {};
static RECT s_playerShowVisualizerToggleRect = {};
static RECT s_playerColorPillRects[4] = {};
static RECT s_playerResetFavBtnRect = {};

// Actions Tab hit rects
static RECT s_actRecAudioToggleRect = {};
static RECT s_actAutoDropToggleRect = {};
static RECT s_actTakeShotBtnRect = {};
static RECT s_actSnipBtnRect = {};
static RECT s_actOpenCapturesBtnRect = {};
static RECT s_actOpenShotsBtnRect = {};

// General Tab hit rects
static RECT s_autostartToggleRect = {};
static RECT s_alwaysOnTopToggleRect = {};
static RECT s_autoremoveToggleRect = {};
static RECT s_movefilesToggleRect = {};

// Tray Tab hit rects
static RECT s_edgePillRects[3] = {};
static RECT s_centerTrayBtnRect = {};

// Quick Controls Tab hit rects
static RECT s_qcBrightnessToggleRect = {};
static RECT s_qcVolumeToggleRect = {};
static RECT s_qcWifiToggleRect = {};
static RECT s_qcBtToggleRect = {};
static RECT s_qcAirplaneToggleRect = {};
static RECT s_qcHotspotToggleRect = {};
static RECT s_qcNightLightToggleRect = {};
static RECT s_qcFocusToggleRect = {};
static RECT s_qcMuteToggleRect = {};
static RECT s_qcBatteryToggleRect = {};
static RECT s_qcLockPCToggleRect = {};
static RECT s_qcPowerModeToggleRect = {};
static RECT s_powerPillRects[3] = {};

// About Tab hit rects
static RECT s_checkUpdatesBtnRect = {};
static RECT s_openConfigBtnRect = {};
static RECT s_resetDefaultsBtnRect = {};

void InitSettingsPath()
{
    wchar_t appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
    {
        std::wstring dir = std::wstring(appData) + L"\\WinTray";
        CreateDirectoryW(dir.c_str(), nullptr);
        g_iniPath = dir + L"\\WinTray.ini";
    }
}

void LoadSettings()
{
    if (g_iniPath.empty())
        return;
    g_settings.themeMode = GetPrivateProfileIntW(L"UI", L"ThemeMode", 0, g_iniPath.c_str());
    g_settings.backdropStyle = GetPrivateProfileIntW(L"UI", L"BackdropStyle", 0, g_iniPath.c_str());
    g_settings.notchStyle = GetPrivateProfileIntW(L"UI", L"NotchStyle", NOTCH_ATTACHED, g_iniPath.c_str());
    g_settings.collapsedStyle = GetPrivateProfileIntW(L"UI", L"CollapsedStyle", COLLAPSED_DYNAMIC, g_iniPath.c_str());
    g_settings.recIncludeAudio = GetPrivateProfileIntW(L"Actions", L"RecordAudio", 1, g_iniPath.c_str()) == 1;
    g_settings.recAutoDropInTray = GetPrivateProfileIntW(L"Actions", L"AutoDropInTray", 1, g_iniPath.c_str()) == 1;
    g_settings.moveFiles = GetPrivateProfileIntW(L"Preferences", L"MoveFiles", 0, g_iniPath.c_str()) == 1;
    g_settings.autoRemoveUnpinned = GetPrivateProfileIntW(L"Preferences", L"AutoRemoveUnpinned", 1, g_iniPath.c_str()) == 1;
    g_settings.enableMagnify = GetPrivateProfileIntW(L"Accessibility", L"EnableMagnify", 1, g_iniPath.c_str()) == 1;
    g_settings.largeIcons = GetPrivateProfileIntW(L"Accessibility", L"LargeIcons", 0, g_iniPath.c_str()) == 1;
    g_settings.alwaysOnTop = GetPrivateProfileIntW(L"Window", L"AlwaysOnTop", 1, g_iniPath.c_str()) == 1;
    g_settings.hotkeyMods = GetPrivateProfileIntW(L"Hotkeys", L"Mods", MOD_WIN | MOD_ALT, g_iniPath.c_str());
    g_settings.hotkeyVk = GetPrivateProfileIntW(L"Hotkeys", L"Vk", 'T', g_iniPath.c_str());
    g_settings.dockEdge = GetPrivateProfileIntW(L"Window", L"DockEdge", EDGE_TOP, g_iniPath.c_str());

    // Quick Controls customization
    g_settings.qcShowWifi = GetPrivateProfileIntW(L"QuickControls", L"ShowWifi", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowBluetooth = GetPrivateProfileIntW(L"QuickControls", L"ShowBluetooth", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowAirplane = GetPrivateProfileIntW(L"QuickControls", L"ShowAirplane", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowHotspot = GetPrivateProfileIntW(L"QuickControls", L"ShowHotspot", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowNightLight = GetPrivateProfileIntW(L"QuickControls", L"ShowNightLight", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowFocus = GetPrivateProfileIntW(L"QuickControls", L"ShowFocus", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowMute = GetPrivateProfileIntW(L"QuickControls", L"ShowMute", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowBattery = GetPrivateProfileIntW(L"QuickControls", L"ShowBattery", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowLockPC = GetPrivateProfileIntW(L"QuickControls", L"ShowLockPC", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowBrightness = GetPrivateProfileIntW(L"QuickControls", L"ShowBrightness", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowVolume = GetPrivateProfileIntW(L"QuickControls", L"ShowVolume", 1, g_iniPath.c_str()) == 1;
    g_settings.qcShowPowerMode = GetPrivateProfileIntW(L"QuickControls", L"ShowPowerMode", 1, g_iniPath.c_str()) == 1;

    // Player preferences
    g_settings.showMusicTab = GetPrivateProfileIntW(L"Player", L"ShowMusicTab", 1, g_iniPath.c_str()) == 1;
    g_settings.playerShowCollapsedArt = GetPrivateProfileIntW(L"Player", L"ShowCollapsedArt", 1, g_iniPath.c_str()) == 1;
    g_settings.playerShowVisualizer = GetPrivateProfileIntW(L"Player", L"ShowVisualizer", 1, g_iniPath.c_str()) == 1;
    g_settings.playerVisualizerColor = GetPrivateProfileIntW(L"Player", L"VisualizerColor", 0, g_iniPath.c_str());
}

void SaveSettings()
{
    if (g_iniPath.empty())
        return;
    WritePrivateProfileStringW(L"UI", L"ThemeMode", std::to_wstring(g_settings.themeMode).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"UI", L"BackdropStyle", std::to_wstring(g_settings.backdropStyle).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"UI", L"NotchStyle", std::to_wstring(g_settings.notchStyle).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"UI", L"CollapsedStyle", std::to_wstring(g_settings.collapsedStyle).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"Actions", L"RecordAudio", g_settings.recIncludeAudio ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Actions", L"AutoDropInTray", g_settings.recAutoDropInTray ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Preferences", L"MoveFiles", g_settings.moveFiles ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Preferences", L"AutoRemoveUnpinned", g_settings.autoRemoveUnpinned ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Accessibility", L"EnableMagnify", g_settings.enableMagnify ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Accessibility", L"LargeIcons", g_settings.largeIcons ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Window", L"AlwaysOnTop", g_settings.alwaysOnTop ? L"1" : L"0", g_iniPath.c_str());

    wchar_t buf[32];
    swprintf_s(buf, L"%d", g_settings.hotkeyMods);
    WritePrivateProfileStringW(L"Hotkeys", L"Mods", buf, g_iniPath.c_str());
    swprintf_s(buf, L"%d", g_settings.hotkeyVk);
    WritePrivateProfileStringW(L"Hotkeys", L"Vk", buf, g_iniPath.c_str());
    swprintf_s(buf, L"%d", g_settings.dockEdge);
    WritePrivateProfileStringW(L"Window", L"DockEdge", buf, g_iniPath.c_str());

    // Quick controls
    WritePrivateProfileStringW(L"QuickControls", L"ShowWifi", g_settings.qcShowWifi ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowBluetooth", g_settings.qcShowBluetooth ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowAirplane", g_settings.qcShowAirplane ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowHotspot", g_settings.qcShowHotspot ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowNightLight", g_settings.qcShowNightLight ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowFocus", g_settings.qcShowFocus ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowMute", g_settings.qcShowMute ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowBattery", g_settings.qcShowBattery ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowLockPC", g_settings.qcShowLockPC ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowBrightness", g_settings.qcShowBrightness ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowVolume", g_settings.qcShowVolume ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"QuickControls", L"ShowPowerMode", g_settings.qcShowPowerMode ? L"1" : L"0", g_iniPath.c_str());

    // Player preferences
    WritePrivateProfileStringW(L"Player", L"ShowMusicTab", g_settings.showMusicTab ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Player", L"ShowCollapsedArt", g_settings.playerShowCollapsedArt ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Player", L"ShowVisualizer", g_settings.playerShowVisualizer ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Player", L"VisualizerColor", std::to_wstring(g_settings.playerVisualizerColor).c_str(), g_iniPath.c_str());
}

static bool IsAutoStartEnabled()
{
    HKEY hKey;
    const wchar_t *subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    bool enabled = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        enabled = (RegQueryValueExW(hKey, L"WinTray", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS);
        RegCloseKey(hKey);
    }
    return enabled;
}

static void SetAutoStart(bool enable)
{
    HKEY hKey;
    const wchar_t *subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (enable)
        {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            RegSetValueExW(hKey, L"WinTray", 0, REG_SZ, reinterpret_cast<const BYTE *>(exePath), static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(hKey, L"WinTray");
        }
        RegCloseKey(hKey);
    }
}

static std::wstring GetHotkeyDisplayString(int mods, int vk)
{
    std::wstring s;
    if (mods & MOD_WIN) s += L"Win + ";
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT) s += L"Alt + ";
    if (mods & MOD_SHIFT) s += L"Shift + ";

    if (vk >= 'A' && vk <= 'Z')
    {
        s += static_cast<wchar_t>(vk);
    }
    else if (vk >= '0' && vk <= '9')
    {
        s += static_cast<wchar_t>(vk);
    }
    else if (vk >= VK_F1 && vk <= VK_F12)
    {
        s += L"F" + std::to_wstring(vk - VK_F1 + 1);
    }
    else if (vk == VK_SPACE)
    {
        s += L"Space";
    }
    else if (vk == VK_TAB)
    {
        s += L"Tab";
    }
    else
    {
        wchar_t keyName[64] = {};
        UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (GetKeyNameTextW(scanCode << 16, keyName, 64) > 0)
            s += keyName;
        else
            s += L"Key " + std::to_wstring(vk);
    }
    return s;
}

static HFONT CreateClearTypeFont(int sizePx, int weight, const wchar_t *fontFamily = L"Segoe UI")
{
    return CreateFontW(-sizePx, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fontFamily);
}

static void DrawRoundedRect(HDC hdc, const RECT &r, int radius, COLORREF bg, COLORREF border, int borderWidth = 1)
{
    HBRUSH brush = CreateSolidBrush(bg);
    HPEN pen = (borderWidth > 0) ? CreatePen(PS_SOLID, borderWidth, border) : CreatePen(PS_NULL, 0, 0);
    HGDIOBJ oldB = SelectObject(hdc, brush);
    HGDIOBJ oldP = SelectObject(hdc, pen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(brush);
    DeleteObject(pen);
}

static void DrawToggleSwitch(HDC hdc, const RECT &r, bool isOn, bool isDark)
{
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int radius = h / 2;

    COLORREF trackBg, trackBorder, knobColor;
    if (isOn)
    {
        trackBg = RGB(24, 120, 242);
        trackBorder = RGB(20, 100, 210);
        knobColor = RGB(255, 255, 255);
    }
    else
    {
        trackBg = isDark ? RGB(40, 44, 56) : RGB(218, 224, 235);
        trackBorder = isDark ? RGB(65, 72, 90) : RGB(185, 195, 210);
        knobColor = isDark ? RGB(200, 205, 220) : RGB(255, 255, 255);
    }

    DrawRoundedRect(hdc, r, radius, trackBg, trackBorder, 1);

    int knobMargin = 3;
    int knobD = h - knobMargin * 2;
    int knobX = isOn ? (r.right - knobMargin - knobD) : (r.left + knobMargin);
    int knobY = r.top + knobMargin;

    RECT knobRc = {knobX, knobY, knobX + knobD, knobY + knobD};
    DrawRoundedRect(hdc, knobRc, knobD / 2, knobColor, knobColor, 0);
}

static void DrawSegmentedPills(HDC hdc, const RECT &r, const std::vector<std::wstring> &options, int selectedIdx, RECT *outPillRects, bool isDark)
{
    int count = static_cast<int>(options.size());
    if (count == 0) return;

    COLORREF containerBg = isDark ? RGB(30, 33, 42) : RGB(225, 232, 242);
    COLORREF containerBorder = isDark ? RGB(50, 56, 70) : RGB(195, 205, 220);
    DrawRoundedRect(hdc, r, 16, containerBg, containerBorder, 1);

    int totalW = r.right - r.left - 4;
    int pillW = totalW / count;
    int pillH = r.bottom - r.top - 4;

    HFONT font = CreateClearTypeFont(12, FW_SEMIBOLD, L"Segoe UI");
    HGDIOBJ oldF = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);

    for (int i = 0; i < count; ++i)
    {
        int pLeft = r.left + 2 + i * pillW;
        int pRight = (i == count - 1) ? (r.right - 2) : (pLeft + pillW);
        RECT pRc = {pLeft, r.top + 2, pRight, r.top + 2 + pillH};

        if (outPillRects)
            outPillRects[i] = pRc;

        bool isSel = (i == selectedIdx);
        if (isSel)
        {
            COLORREF selBg = isDark ? RGB(246, 248, 252) : RGB(26, 32, 44);
            DrawRoundedRect(hdc, pRc, 14, selBg, selBg, 0);
            SetTextColor(hdc, isDark ? RGB(16, 20, 26) : RGB(255, 255, 255));
        }
        else
        {
            SetTextColor(hdc, isDark ? RGB(160, 170, 188) : RGB(65, 75, 95));
        }

        DrawTextW(hdc, options[i].c_str(), -1, &pRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }

    SelectObject(hdc, oldF);
    DeleteObject(font);
}

static void DrawModernActionBtn(HDC hdc, const RECT &r, const std::wstring &text, bool isDark, const wchar_t *iconGlyph = nullptr)
{
    COLORREF btnBg = isDark ? RGB(36, 40, 52) : RGB(225, 232, 244);
    COLORREF btnBorder = isDark ? RGB(60, 68, 86) : RGB(190, 202, 220);
    DrawRoundedRect(hdc, r, 8, btnBg, btnBorder, 1);

    HFONT textFont = CreateClearTypeFont(12, FW_SEMIBOLD, L"Segoe UI");
    HGDIOBJ oldF = SelectObject(hdc, textFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, isDark ? RGB(240, 244, 255) : RGB(22, 28, 40));

    if (iconGlyph)
    {
        HFONT iconFont = CreateClearTypeFont(13, FW_NORMAL, L"Segoe MDL2 Assets");
        RECT iRc = {r.left + 10, r.top, r.left + 28, r.bottom};
        SelectObject(hdc, iconFont);
        DrawTextW(hdc, iconGlyph, -1, &iRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(hdc, textFont);
        DeleteObject(iconFont);

        RECT tRc = {r.left + 30, r.top, r.right - 10, r.bottom};
        DrawTextW(hdc, text.c_str(), -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    }
    else
    {
        RECT tRc = r;
        DrawTextW(hdc, text.c_str(), -1, &tRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }

    SelectObject(hdc, oldF);
    DeleteObject(textFont);
}

static void DrawLivePreviewCard(HDC hdc, const RECT &r, bool isOpen, bool isDark)
{
    COLORREF gStart = isDark ? RGB(26, 32, 54) : RGB(195, 215, 245);
    COLORREF borderCol = isDark ? RGB(65, 75, 105) : RGB(180, 195, 225);
    DrawRoundedRect(hdc, r, 16, gStart, borderCol, 1);

    int cX = (r.left + r.right) / 2;
    if (!isOpen)
    {
        int nW = 130, nH = 26;
        int nX = cX - nW / 2;
        int nY = r.top;
        RECT notchRc = {nX, nY, nX + nW, nY + nH};
        DrawRoundedRect(hdc, notchRc, 12, RGB(10, 12, 16), RGB(28, 32, 42), 1);

        COLORREF dotCol = RGB(250, 160, 40);
        HBRUSH dotB = CreateSolidBrush(dotCol);
        HGDIOBJ oldB = SelectObject(hdc, dotB);
        HPEN noPen = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ oldP = SelectObject(hdc, noPen);
        for (int i = 0; i < 3; ++i)
        {
            int h = (i == 1 ? 12 : 8);
            RoundRect(hdc, notchRc.right - 24 + i * 5, notchRc.top + (nH - h) / 2, notchRc.right - 21 + i * 5, notchRc.top + (nH + h) / 2, 2, 2);
        }
        SelectObject(hdc, oldB);
        SelectObject(hdc, oldP);
        DeleteObject(dotB);
        DeleteObject(noPen);

        RECT artRc = {notchRc.left + 8, notchRc.top + 4, notchRc.left + 24, notchRc.top + 20};
        DrawRoundedRect(hdc, artRc, 4, RGB(24, 120, 242), RGB(20, 90, 190), 0);
    }
    else
    {
        int nW = 240, nH = 70;
        int nX = cX - nW / 2;
        int nY = r.top + 4;
        RECT cardRc = {nX, nY, nX + nW, nY + nH};
        DrawRoundedRect(hdc, cardRc, 18, RGB(14, 16, 22), RGB(40, 46, 62), 1);

        RECT artRc = {cardRc.left + 12, cardRc.top + 10, cardRc.left + 50, cardRc.top + 48};
        DrawRoundedRect(hdc, artRc, 6, RGB(24, 120, 242), RGB(20, 90, 190), 0);

        HFONT miniF = CreateClearTypeFont(11, FW_SEMIBOLD, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(hdc, miniF);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        RECT tRc = {cardRc.left + 58, cardRc.top + 12, cardRc.right - 12, cardRc.top + 26};
        DrawTextW(hdc, L"Project_Report.pdf", -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        RECT subRc = {cardRc.left + 58, cardRc.top + 28, cardRc.right - 12, cardRc.top + 40};
        SetTextColor(hdc, RGB(140, 150, 170));
        DrawTextW(hdc, L"Pinned - Ready to share", -1, &subRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        RECT progRc = {cardRc.left + 58, cardRc.top + 48, cardRc.right - 16, cardRc.top + 52};
        DrawRoundedRect(hdc, progRc, 2, RGB(45, 52, 68), RGB(45, 52, 68), 0);
        RECT fillRc = {progRc.left, progRc.top, progRc.left + 76, progRc.bottom};
        DrawRoundedRect(hdc, fillRc, 2, RGB(24, 120, 242), RGB(24, 120, 242), 0);

        SelectObject(hdc, oldF);
        DeleteObject(miniF);
    }

    HFONT capFont = CreateClearTypeFont(11, FW_NORMAL, L"Segoe UI");
    HGDIOBJ oldCF = SelectObject(hdc, capFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, isDark ? RGB(200, 215, 240) : RGB(65, 75, 95));
    RECT capRc = {r.left + 16, r.bottom - 26, r.left + 380, r.bottom - 6};
    DrawTextW(hdc, L"Live preview. Hover over the dock to inspect files and controls.", -1, &capRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    SelectObject(hdc, oldCF);
    DeleteObject(capFont);

    s_previewToggleRect = {r.right - 146, r.bottom - 32, r.right - 14, r.bottom - 8};
    std::vector<std::wstring> opts = {L"Resting", L"Open"};
    DrawSegmentedPills(hdc, s_previewToggleRect, opts, isOpen ? 1 : 0, nullptr, isDark);
}

static void PerformSearchFilter(HWND hwnd)
{
    std::wstring q = s_searchQuery;
    std::transform(q.begin(), q.end(), q.begin(), ::towlower);
    if (!q.empty())
    {
        if (q.find(L"music") != std::wstring::npos || q.find(L"player") != std::wstring::npos ||
            q.find(L"song") != std::wstring::npos || q.find(L"spotify") != std::wstring::npos ||
            q.find(L"visualizer") != std::wstring::npos || q.find(L"fav") != std::wstring::npos)
        {
            s_activeTab = STAB_PLAYER;
        }
        else if (q.find(L"record") != std::wstring::npos || q.find(L"shot") != std::wstring::npos ||
            q.find(L"screen") != std::wstring::npos || q.find(L"snip") != std::wstring::npos ||
            q.find(L"action") != std::wstring::npos || q.find(L"video") != std::wstring::npos ||
            q.find(L"capture") != std::wstring::npos)
        {
            s_activeTab = STAB_ACTIONS;
        }
        else if (q.find(L"collapse") != std::wstring::npos || q.find(L"clock") != std::wstring::npos ||
                 q.find(L"equalizer") != std::wstring::npos ||
                 q.find(L"theme") != std::wstring::npos || q.find(L"dark") != std::wstring::npos ||
                 q.find(L"light") != std::wstring::npos || q.find(L"glass") != std::wstring::npos ||
                 q.find(L"surface") != std::wstring::npos || q.find(L"icon") != std::wstring::npos ||
                 q.find(L"magnif") != std::wstring::npos || q.find(L"appear") != std::wstring::npos)
        {
            s_activeTab = STAB_APPEARANCE;
        }
        else if (q.find(L"dock") != std::wstring::npos || q.find(L"edge") != std::wstring::npos ||
                 q.find(L"top") != std::wstring::npos || q.find(L"left") != std::wstring::npos ||
                 q.find(L"right") != std::wstring::npos || q.find(L"notch") != std::wstring::npos ||
                 q.find(L"attach") != std::wstring::npos || q.find(L"float") != std::wstring::npos ||
                 q.find(L"center") != std::wstring::npos)
        {
            s_activeTab = STAB_TRAY;
        }
        else if (q.find(L"wifi") != std::wstring::npos || q.find(L"blue") != std::wstring::npos ||
                 q.find(L"air") != std::wstring::npos || q.find(L"hot") != std::wstring::npos ||
                 q.find(L"night") != std::wstring::npos || q.find(L"focus") != std::wstring::npos ||
                 q.find(L"dnd") != std::wstring::npos || q.find(L"mute") != std::wstring::npos ||
                 q.find(L"sound") != std::wstring::npos || q.find(L"battery") != std::wstring::npos ||
                 q.find(L"slider") != std::wstring::npos || q.find(L"bright") != std::wstring::npos ||
                 q.find(L"vol") != std::wstring::npos || q.find(L"control") != std::wstring::npos ||
                 q.find(L"power") != std::wstring::npos)
        {
            s_activeTab = STAB_CONTROLS;
        }
        else if (q.find(L"start") != std::wstring::npos || q.find(L"boot") != std::wstring::npos ||
                 q.find(L"hotkey") != std::wstring::npos || q.find(L"shortcut") != std::wstring::npos ||
                 q.find(L"file") != std::wstring::npos || q.find(L"unpin") != std::wstring::npos ||
                 q.find(L"top") != std::wstring::npos || q.find(L"general") != std::wstring::npos)
        {
            s_activeTab = STAB_GENERAL;
        }
        else if (q.find(L"about") != std::wstring::npos || q.find(L"update") != std::wstring::npos ||
                 q.find(L"reset") != std::wstring::npos || q.find(L"version") != std::wstring::npos ||
                 q.find(L"ini") != std::wstring::npos)
        {
            s_activeTab = STAB_ABOUT;
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void PaintSettingsWindow(HWND hwnd, HDC hdc)
{
    RECT clientRc;
    GetClientRect(hwnd, &clientRc);
    int width = clientRc.right;
    int height = clientRc.bottom;

    bool isDark = IsResolvedDarkMode();

    HDC memDC = CreateCompatibleDC(hdc);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *pBits = nullptr;
    HBITMAP memBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(memDC, memBitmap);

    // 1. Initialize background:
    // In Dark Mode: pure RGB(0,0,0) allows DWM Acrylic blur to shine through natively.
    // In Light Mode: pre-fill with the exact light surface tint so GDI ClearType anti-aliases
    // text smoothly against light glass instead of pitch black, eliminating all jagged edges!
    COLORREF sideBg = isDark ? RGB(0, 0, 0) : RGB(236, 240, 248);
    COLORREF mainBg = isDark ? RGB(0, 0, 0) : RGB(246, 249, 254);
    DWORD sideBgDword = (GetRValue(sideBg) << 16) | (GetGValue(sideBg) << 8) | GetBValue(sideBg);
    DWORD mainBgDword = (GetRValue(mainBg) << 16) | (GetGValue(mainBg) << 8) | GetBValue(mainBg);

    RECT sideRc = {0, 0, 210, height};
    HBRUSH sideBr = CreateSolidBrush(sideBg);
    FillRect(memDC, &sideRc, sideBr);
    DeleteObject(sideBr);

    RECT mainRc = {210, 0, width, height};
    HBRUSH mainBr = CreateSolidBrush(mainBg);
    FillRect(memDC, &mainRc, mainBr);
    DeleteObject(mainBr);

    // Sidebar vertical divider line
    COLORREF divCol = isDark ? RGB(50, 56, 74) : RGB(205, 215, 230);
    HPEN divPen = CreatePen(PS_SOLID, 1, divCol);
    HGDIOBJ oldP = SelectObject(memDC, divPen);
    MoveToEx(memDC, 210, 0, nullptr);
    LineTo(memDC, 210, height);
    SelectObject(memDC, oldP);
    DeleteObject(divPen);

    // ==========================================
    // 1. SIDEBAR (Left Pane)
    // ==========================================
    HFONT logoFont = CreateClearTypeFont(16, FW_BOLD, L"Segoe UI");
    HGDIOBJ oldF = SelectObject(memDC, logoFont);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, isDark ? RGB(255, 255, 255) : RGB(16, 20, 28));
    RECT titleRc = {20, 16, 190, 42};
    DrawTextW(memDC, L"WinTray Settings", -1, &titleRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    SelectObject(memDC, oldF);
    DeleteObject(logoFont);

    // Native Search Pill Bar (No white edit box, No broken glyphs)
    s_searchBoxRect = {16, 50, 194, 82};
    COLORREF sBoxBg = isDark ? RGB(28, 31, 40) : RGB(226, 233, 244);
    COLORREF sBoxBorder = s_searchFocused ? RGB(24, 120, 242) : (isDark ? RGB(50, 56, 72) : RGB(200, 210, 226));
    DrawRoundedRect(memDC, s_searchBoxRect, 14, sBoxBg, sBoxBorder, s_searchFocused ? 2 : 1);

    HFONT searchIconFont = CreateClearTypeFont(12, FW_NORMAL, L"Segoe MDL2 Assets");
    oldF = SelectObject(memDC, searchIconFont);
    SetTextColor(memDC, isDark ? RGB(140, 150, 170) : RGB(100, 110, 125));
    RECT sIconRc = {24, 50, 44, 82};
    DrawTextW(memDC, L"\xE721", -1, &sIconRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    HFONT searchFont = CreateClearTypeFont(12, FW_NORMAL, L"Segoe UI");
    SelectObject(memDC, searchFont);

    RECT sTextRc = {46, 50, s_searchQuery.empty() ? 186 : 166, 82};
    if (s_searchQuery.empty())
    {
        SetTextColor(memDC, isDark ? RGB(140, 150, 170) : RGB(120, 130, 145));
        DrawTextW(memDC, L"Search settings...", -1, &sTextRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        s_searchClearRect = {0, 0, 0, 0};
    }
    else
    {
        SetTextColor(memDC, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
        DrawTextW(memDC, s_searchQuery.c_str(), -1, &sTextRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        if (s_searchFocused && s_caretVisible)
        {
            SIZE sz = {};
            GetTextExtentPoint32W(memDC, s_searchQuery.c_str(), static_cast<int>(s_searchQuery.size()), &sz);
            int cursorX = sTextRc.left + sz.cx + 2;
            HPEN curPen = CreatePen(PS_SOLID, 1, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
            HGDIOBJ op = SelectObject(memDC, curPen);
            MoveToEx(memDC, cursorX, 58, nullptr);
            LineTo(memDC, cursorX, 74);
            SelectObject(memDC, op);
            DeleteObject(curPen);
        }

        s_searchClearRect = {168, 52, 190, 80};
        SelectObject(memDC, searchIconFont);
        SetTextColor(memDC, isDark ? RGB(170, 180, 200) : RGB(80, 90, 110));
        DrawTextW(memDC, L"\xE8BB", -1, &s_searchClearRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    SelectObject(memDC, oldF);
    DeleteObject(searchFont);
    DeleteObject(searchIconFont);

    // Tab Items
    struct TabItemDef
    {
        SettingsTab tab;
        std::wstring title;
        const wchar_t *icon;
    };

    TabItemDef tabs[STAB_COUNT] = {
        {STAB_GENERAL, L"General", L"\xE713"},
        {STAB_APPEARANCE, L"Appearance", L"\xE790"},
        {STAB_PLAYER, L"Music Player", L"\xE8D6"},
        {STAB_ACTIONS, L"Actions", L"\xE722"},
        {STAB_TRAY, L"Tray & Dock", L"\xE737"},
        {STAB_CONTROLS, L"Quick Controls", L"\xE706"},
        {STAB_ABOUT, L"About", L"\xE946"}
    };

    HFONT tabTextFont = CreateClearTypeFont(13, FW_SEMIBOLD, L"Segoe UI");
    HFONT tabIconFont = CreateClearTypeFont(15, FW_NORMAL, L"Segoe MDL2 Assets");

    for (int i = 0; i < STAB_COUNT; ++i)
    {
        int tY = 96 + i * 42;
        s_tabRects[i] = {16, tY, 194, tY + 38};

        bool isActive = (s_activeTab == tabs[i].tab);
        bool isHover = (s_hoveredTab == i);

        if (isActive)
        {
            COLORREF aBg = isDark ? RGB(34, 38, 48) : RGB(222, 230, 242);
            DrawRoundedRect(memDC, s_tabRects[i], 12, aBg, aBg, 0);

            HBRUSH dotB = CreateSolidBrush(RGB(24, 120, 242));
            HPEN noP = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ ob = SelectObject(memDC, dotB);
            HGDIOBJ op = SelectObject(memDC, noP);
            RoundRect(memDC, s_tabRects[i].right - 8, tY + 16, s_tabRects[i].right - 4, tY + 22, 4, 4);
            SelectObject(memDC, ob);
            SelectObject(memDC, op);
            DeleteObject(dotB);
            DeleteObject(noP);
        }
        else if (isHover)
        {
            COLORREF hBg = isDark ? RGB(26, 29, 38) : RGB(232, 238, 246);
            DrawRoundedRect(memDC, s_tabRects[i], 12, hBg, hBg, 0);
        }

        SelectObject(memDC, tabIconFont);
        SetTextColor(memDC, isActive ? (isDark ? RGB(255, 255, 255) : RGB(18, 22, 32))
                                     : (isDark ? RGB(150, 160, 178) : RGB(90, 100, 115)));
        RECT iRc = {s_tabRects[i].left + 12, s_tabRects[i].top, s_tabRects[i].left + 32, s_tabRects[i].bottom};
        DrawTextW(memDC, tabs[i].icon, -1, &iRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        SelectObject(memDC, tabTextFont);
        RECT lRc = {s_tabRects[i].left + 38, s_tabRects[i].top, s_tabRects[i].right - 14, s_tabRects[i].bottom};
        DrawTextW(memDC, tabs[i].title.c_str(), -1, &lRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    }

    DeleteObject(tabTextFont);
    DeleteObject(tabIconFont);

    // ==========================================
    // 2. CLOSE BUTTON (Top-Right)
    // ==========================================
    s_closeBtnRect = {width - 38, 12, width - 12, 38};
    if (s_closeHovered)
    {
        COLORREF chBg = isDark ? RGB(180, 40, 50) : RGB(240, 60, 70);
        DrawRoundedRect(memDC, s_closeBtnRect, 8, chBg, chBg, 0);
    }
    HFONT closeFont = CreateClearTypeFont(14, FW_NORMAL, L"Segoe MDL2 Assets");
    oldF = SelectObject(memDC, closeFont);
    SetTextColor(memDC, s_closeHovered ? RGB(255, 255, 255) : (isDark ? RGB(160, 170, 185) : RGB(90, 100, 115)));
    DrawTextW(memDC, L"\xE8BB", -1, &s_closeBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    SelectObject(memDC, oldF);
    DeleteObject(closeFont);

    // ==========================================
    // 3. CONTENT PANE (Right Pane)
    // ==========================================
    int startX = 236;
    int rightEdge = width - 24;

    HFONT secTitleFont = CreateClearTypeFont(15, FW_BOLD, L"Segoe UI");
    HFONT descFont = CreateClearTypeFont(11, FW_NORMAL, L"Segoe UI");

    auto drawRowHeader = [&](int y, const wchar_t *title, const wchar_t *desc) {
        SelectObject(memDC, secTitleFont);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, isDark ? RGB(255, 255, 255) : RGB(18, 22, 32));
        RECT tRc = {startX, y, rightEdge - 280, y + 22};
        DrawTextW(memDC, title, -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        SelectObject(memDC, descFont);
        SetTextColor(memDC, isDark ? RGB(145, 155, 175) : RGB(90, 100, 115));
        RECT dRc = {startX, y + 24, rightEdge - 280, y + 66};
        DrawTextW(memDC, desc, -1, &dRc, DT_WORDBREAK | DT_LEFT);
    };

    auto drawControlCard = [&](const RECT &r, const wchar_t *glyph, const wchar_t *title, const wchar_t *desc, bool isEnabled, RECT &outToggle)
    {
        COLORREF cardBg = isDark ? RGB(26, 29, 38) : RGB(232, 238, 246);
        COLORREF cardBorder = isDark ? RGB(45, 50, 64) : RGB(205, 215, 230);
        DrawRoundedRect(memDC, r, 10, cardBg, cardBorder, 1);

        int iconD = 28;
        RECT bRc = {r.left + 10, r.top + (r.bottom - r.top - iconD) / 2, r.left + 10 + iconD, r.top + (r.bottom - r.top + iconD) / 2};
        COLORREF badgeBg = isEnabled ? RGB(24, 120, 242) : (isDark ? RGB(40, 44, 56) : RGB(208, 216, 228));
        COLORREF badgeCol = isEnabled ? RGB(255, 255, 255) : (isDark ? RGB(160, 170, 185) : RGB(70, 80, 95));
        DrawRoundedRect(memDC, bRc, 14, badgeBg, badgeBg, 0);

        HFONT iFont = CreateClearTypeFont(14, FW_NORMAL, L"Segoe MDL2 Assets");
        HGDIOBJ oi = SelectObject(memDC, iFont);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, badgeCol);
        DrawTextW(memDC, glyph, -1, &bRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(memDC, oi);
        DeleteObject(iFont);

        HFONT tFont = CreateClearTypeFont(12, FW_SEMIBOLD, L"Segoe UI");
        HGDIOBJ ot = SelectObject(memDC, tFont);
        SetTextColor(memDC, isDark ? RGB(255, 255, 255) : RGB(18, 22, 32));
        RECT tRc = {r.left + 46, r.top + 8, r.right - 54, r.top + 24};
        DrawTextW(memDC, title, -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        SelectObject(memDC, ot);
        DeleteObject(tFont);

        HFONT dFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe UI");
        HGDIOBJ od = SelectObject(memDC, dFont);
        SetTextColor(memDC, isDark ? RGB(140, 150, 168) : RGB(95, 105, 120));
        RECT dRc = {r.left + 46, r.top + 24, r.right - 54, r.top + 42};
        DrawTextW(memDC, desc, -1, &dRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        SelectObject(memDC, od);
        DeleteObject(dFont);

        outToggle = {r.right - 46, r.top + (r.bottom - r.top - 20) / 2, r.right - 10, r.top + (r.bottom - r.top + 20) / 2};
        DrawToggleSwitch(memDC, outToggle, isEnabled, isDark);
    };

    if (s_activeTab == STAB_APPEARANCE)
    {
        // Live Preview Box
        s_previewBoxRect = {startX, 44, rightEdge, 154};
        DrawLivePreviewCard(memDC, s_previewBoxRect, s_previewOpen, isDark);

        // Setting 1: Theme Mode
        int y1 = 174;
        drawRowHeader(y1, L"Theme", L"Select your color palette preference.");
        RECT tPillRc = {rightEdge - 270, y1 + 8, rightEdge, y1 + 40};
        std::vector<std::wstring> themeOpts = {L"System", L"Light", L"Dark"};
        DrawSegmentedPills(memDC, tPillRc, themeOpts, g_settings.themeMode, s_themePillRects, isDark);

        // Setting 2: Surface Glass
        int y2 = 246;
        drawRowHeader(y2, L"Surface", L"Liquid Glass blurs wallpaper; Tinted adds soft color.");
        RECT bPillRc = {rightEdge - 270, y2 + 8, rightEdge, y2 + 40};
        std::vector<std::wstring> backOpts = {L"Liquid Glass", L"Tinted Candy"};
        DrawSegmentedPills(memDC, bPillRc, backOpts, g_settings.backdropStyle, s_backdropPillRects, isDark);

        // Setting 3: Collapsed View Customization
        int y3 = 318;
        drawRowHeader(y3, L"Collapsed View", L"Resting dock appearance: grab line, clock, music bars, or dynamic HUD.");
        RECT cPillRc = {rightEdge - 340, y3 + 8, rightEdge, y3 + 40};
        std::vector<std::wstring> colOpts = {L"Pill", L"Clock", L"Equalizer", L"Dynamic"};
        DrawSegmentedPills(memDC, cPillRc, colOpts, g_settings.collapsedStyle, s_collapsedPillRects, isDark);

        // Setting 4: Notch Attachment
        int y4 = 384;
        drawRowHeader(y4, L"Notch Style", L"Attached sits on the screen bezel; Floating hovers.");
        RECT nPillRc = {rightEdge - 270, y4 + 8, rightEdge, y4 + 40};
        std::vector<std::wstring> notchOpts = {L"Attached", L"Floating"};
        DrawSegmentedPills(memDC, nPillRc, notchOpts, g_settings.notchStyle, s_notchPillRects, isDark);

        // Setting 5: Large Icons
        int y5 = 446;
        drawRowHeader(y5, L"Large Icons", L"Use 40px icon cards for high DPI displays.");
        s_largeIconsToggleRect = {rightEdge - 48, y5 + 12, rightEdge - 4, y5 + 36};
        DrawToggleSwitch(memDC, s_largeIconsToggleRect, g_settings.largeIcons, isDark);

        // Setting 6: Magnification
        int y6 = 506;
        drawRowHeader(y6, L"Cursor Magnification", L"Smooth dock zoom animation on item hover.");
        s_magnifyToggleRect = {rightEdge - 48, y6 + 12, rightEdge - 4, y6 + 36};
        DrawToggleSwitch(memDC, s_magnifyToggleRect, g_settings.enableMagnify, isDark);
    }
    else if (s_activeTab == STAB_GENERAL)
    {
        // Setting 1: Start with Windows
        int y1 = 44;
        drawRowHeader(y1, L"Start with Windows", L"Automatically run WinTray on system startup.");
        s_autostartToggleRect = {rightEdge - 48, y1 + 12, rightEdge - 4, y1 + 36};
        DrawToggleSwitch(memDC, s_autostartToggleRect, IsAutoStartEnabled(), isDark);

        // Setting 2: Always on Top
        int y2 = 120;
        drawRowHeader(y2, L"Always on Top", L"Keep WinTray visible above full-screen and maximized windows.");
        s_alwaysOnTopToggleRect = {rightEdge - 48, y2 + 12, rightEdge - 4, y2 + 36};
        DrawToggleSwitch(memDC, s_alwaysOnTopToggleRect, g_settings.alwaysOnTop, isDark);

        // Setting 3: Auto Remove Unpinned
        int y3 = 196;
        drawRowHeader(y3, L"Auto-remove Unpinned", L"Clear unpinned items when dragged out of the dock.");
        s_autoremoveToggleRect = {rightEdge - 48, y3 + 12, rightEdge - 4, y3 + 36};
        DrawToggleSwitch(memDC, s_autoremoveToggleRect, g_settings.autoRemoveUnpinned, isDark);

        // Setting 4: Move Files
        int y4 = 272;
        drawRowHeader(y4, L"Move Files on Drop", L"Move original files into tray instead of copying.");
        s_movefilesToggleRect = {rightEdge - 48, y4 + 12, rightEdge - 4, y4 + 36};
        DrawToggleSwitch(memDC, s_movefilesToggleRect, g_settings.moveFiles, isDark);

        // Setting 5: Editable Global Hotkey
        int y5 = 348;
        drawRowHeader(y5, L"Global Toggle Hotkey", L"Keyboard shortcut to toggle WinTray. Click to rebind.");
        s_hotkeyBtnRect = {rightEdge - 170, y5 + 8, rightEdge, y5 + 40};

        COLORREF hkBg, hkBorder, hkText;
        std::wstring hkLabel;
        if (s_isRecordingHotkey)
        {
            hkBg = isDark ? RGB(20, 50, 95) : RGB(215, 235, 255);
            hkBorder = RGB(24, 120, 242);
            hkText = isDark ? RGB(100, 180, 255) : RGB(10, 100, 220);
            hkLabel = L"Press shortcut keys...";
        }
        else
        {
            hkBg = isDark ? RGB(32, 36, 48) : RGB(225, 232, 244);
            hkBorder = isDark ? RGB(55, 62, 78) : RGB(190, 202, 220);
            hkText = isDark ? RGB(245, 248, 255) : RGB(20, 26, 38);
            hkLabel = GetHotkeyDisplayString(g_settings.hotkeyMods, g_settings.hotkeyVk);
        }

        DrawRoundedRect(memDC, s_hotkeyBtnRect, 8, hkBg, hkBorder, s_isRecordingHotkey ? 2 : 1);
        HFONT hkFont = CreateClearTypeFont(12, FW_BOLD, L"Segoe UI");
        oldF = SelectObject(memDC, hkFont);
        SetTextColor(memDC, hkText);
        DrawTextW(memDC, hkLabel.c_str(), -1, &s_hotkeyBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(memDC, oldF);
        DeleteObject(hkFont);
    }
    else if (s_activeTab == STAB_PLAYER)
    {
        // Setting 1: Show Music Tab in Dock
        int y1 = 44;
        drawRowHeader(y1, L"Show Music Tab", L"Display the dedicated media player tab in WinTray dock header.");
        s_playerShowMusicTabToggleRect = {rightEdge - 48, y1 + 12, rightEdge - 4, y1 + 36};
        DrawToggleSwitch(memDC, s_playerShowMusicTabToggleRect, g_settings.showMusicTab, isDark);

        // Setting 2: Resting Album Artwork
        int y2 = 120;
        drawRowHeader(y2, L"Resting Album Artwork", L"Show rounded artwork thumbnail and app badge on collapsed dock while playing.");
        s_playerShowArtToggleRect = {rightEdge - 48, y2 + 12, rightEdge - 4, y2 + 36};
        DrawToggleSwitch(memDC, s_playerShowArtToggleRect, g_settings.playerShowCollapsedArt, isDark);

        // Setting 3: Dancing Equalizer Visualizer
        int y3 = 196;
        drawRowHeader(y3, L"Dancing Equalizer", L"Animate live multi-bar audio visualizer on collapsed dock while music plays.");
        s_playerShowVisualizerToggleRect = {rightEdge - 48, y3 + 12, rightEdge - 4, y3 + 36};
        DrawToggleSwitch(memDC, s_playerShowVisualizerToggleRect, g_settings.playerShowVisualizer, isDark);

        // Setting 4: Visualizer Accent Color
        int y4 = 272;
        drawRowHeader(y4, L"Visualizer Color", L"Choose accent color for the animated equalizer bars.");
        RECT cPillRc = {rightEdge - 340, y4 + 8, rightEdge, y4 + 40};
        std::vector<std::wstring> cOpts = {L"Coral Pink", L"Cyan Sky", L"Emerald", L"Amber"};
        DrawSegmentedPills(memDC, cPillRc, cOpts, g_settings.playerVisualizerColor, s_playerColorPillRects, isDark);

        // Setting 5: Stream Favorites & Quick Launch
        int y5 = 356;
        drawRowHeader(y5, L"Stream Favorites", L"Manage quick launch music links and radio streams.");
        s_playerResetFavBtnRect = {rightEdge - 150, y5 + 8, rightEdge, y5 + 40};
        DrawModernActionBtn(memDC, s_playerResetFavBtnRect, L"Reset Favorites", isDark, L"\xE777");
    }
    else if (s_activeTab == STAB_ACTIONS)
    {
        int y1 = 44;
        drawRowHeader(y1, L"Record System Audio", L"Capture speaker and media audio while recording the screen.");
        s_actRecAudioToggleRect = {rightEdge - 48, y1 + 12, rightEdge - 4, y1 + 36};
        DrawToggleSwitch(memDC, s_actRecAudioToggleRect, g_settings.recIncludeAudio, isDark);

        int y2 = 120;
        drawRowHeader(y2, L"Auto-drop into WinTray", L"Instantly add finished screen recordings & screenshots to tray files.");
        s_actAutoDropToggleRect = {rightEdge - 48, y2 + 12, rightEdge - 4, y2 + 36};
        DrawToggleSwitch(memDC, s_actAutoDropToggleRect, g_settings.recAutoDropInTray, isDark);

        int y3 = 196;
        drawRowHeader(y3, L"Instant Screenshot", L"Capture the entire screen and copy to clipboard immediately.");
        s_actTakeShotBtnRect = {rightEdge - 190, y3 + 6, rightEdge, y3 + 42};
        DrawModernActionBtn(memDC, s_actTakeShotBtnRect, L"Capture Full Screen", isDark, L"\xE91B");

        int y4 = 272;
        drawRowHeader(y4, L"Snipping Tool", L"Launch Windows interactive region snip overlay.");
        s_actSnipBtnRect = {rightEdge - 190, y4 + 6, rightEdge, y4 + 42};
        DrawModernActionBtn(memDC, s_actSnipBtnRect, L"Open Snipping Tool", isDark, L"\xE722");

        int y5 = 348;
        drawRowHeader(y5, L"Recordings Folder", L"Open the directory where screen recordings are saved.");
        s_actOpenCapturesBtnRect = {rightEdge - 190, y5 + 6, rightEdge, y5 + 42};
        DrawModernActionBtn(memDC, s_actOpenCapturesBtnRect, L"Open Captures", isDark, L"\xE838");

        int y6 = 424;
        drawRowHeader(y6, L"Screenshots Folder", L"Open the directory where screenshots are saved.");
        s_actOpenShotsBtnRect = {rightEdge - 190, y6 + 6, rightEdge, y6 + 42};
        DrawModernActionBtn(memDC, s_actOpenShotsBtnRect, L"Open Screenshots", isDark, L"\xE838");
    }
    else if (s_activeTab == STAB_TRAY)
    {
        // Setting 1: Dock Position Edge
        int y1 = 44;
        drawRowHeader(y1, L"Dock Position", L"Screen edge where the tray notch is anchored.");
        RECT ePillRc = {rightEdge - 270, y1 + 8, rightEdge, y1 + 40};
        std::vector<std::wstring> edgeOpts = {L"Top Edge", L"Right Edge", L"Left Edge"};
        DrawSegmentedPills(memDC, ePillRc, edgeOpts, g_settings.dockEdge, s_edgePillRects, isDark);

        // Setting 2: Attachment
        int y2 = 120;
        drawRowHeader(y2, L"Attachment", L"Bezel-attached notch or floating detached dock.");
        RECT nPillRc = {rightEdge - 270, y2 + 8, rightEdge, y2 + 40};
        std::vector<std::wstring> notchOpts = {L"Attached", L"Floating"};
        DrawSegmentedPills(memDC, nPillRc, notchOpts, g_settings.notchStyle, s_notchPillRects, isDark);

        // Setting 3: Reset Center
        int y3 = 196;
        drawRowHeader(y3, L"Center Tray", L"Reset tray position to the center of active screen.");
        s_centerTrayBtnRect = {rightEdge - 160, y3 + 8, rightEdge, y3 + 40};
        DrawModernActionBtn(memDC, s_centerTrayBtnRect, L"Center on Screen", isDark, L"\xE737");
    }
    else if (s_activeTab == STAB_CONTROLS)
    {
        int y1 = 40;
        drawRowHeader(y1, L"Quick Controls Customizer", L"Choose and edit which sliders and quick control tiles appear in WinTray.");

        int cardW = (rightEdge - startX - 16) / 2;

        // Section 1: Sliders
        RECT sl1 = {startX, y1 + 54, startX + cardW, y1 + 102};
        drawControlCard(sl1, L"\xE706", L"Brightness Slider", L"Panel & monitor display brightness", g_settings.qcShowBrightness, s_qcBrightnessToggleRect);

        RECT sl2 = {startX + cardW + 16, y1 + 54, rightEdge, y1 + 102};
        drawControlCard(sl2, L"\xE767", L"Volume Slider", L"Master system audio volume", g_settings.qcShowVolume, s_qcVolumeToggleRect);

        // Section 2: Action Tiles
        int gridY = y1 + 114;
        int rowH = 50;

        // Row 1
        RECT t1 = {startX, gridY, startX + cardW, gridY + 46};
        drawControlCard(t1, L"\xE701", L"Wi-Fi", L"Wireless networks & flyout", g_settings.qcShowWifi, s_qcWifiToggleRect);

        RECT t2 = {startX + cardW + 16, gridY, rightEdge, gridY + 46};
        drawControlCard(t2, L"\xE702", L"Bluetooth", L"Wireless accessories & devices", g_settings.qcShowBluetooth, s_qcBtToggleRect);

        // Row 2
        RECT t3 = {startX, gridY + rowH, startX + cardW, gridY + rowH + 46};
        drawControlCard(t3, L"\xE709", L"Airplane Mode", L"Turn off all wireless radios", g_settings.qcShowAirplane, s_qcAirplaneToggleRect);

        RECT t4 = {startX + cardW + 16, gridY + rowH, rightEdge, gridY + rowH + 46};
        drawControlCard(t4, L"\xE88A", L"Mobile Hotspot", L"Share your internet connection", g_settings.qcShowHotspot, s_qcHotspotToggleRect);

        // Row 3
        RECT t5 = {startX, gridY + rowH * 2, startX + cardW, gridY + rowH * 2 + 46};
        drawControlCard(t5, L"\xE708", L"Night Light", L"Warm display temperature", g_settings.qcShowNightLight, s_qcNightLightToggleRect);

        RECT t6 = {startX + cardW + 16, gridY + rowH * 2, rightEdge, gridY + rowH * 2 + 46};
        drawControlCard(t6, L"\xE707", L"Focus / DND", L"Silence notifications and alerts", g_settings.qcShowFocus, s_qcFocusToggleRect);

        // Row 4
        RECT t7 = {startX, gridY + rowH * 3, startX + cardW, gridY + rowH * 3 + 46};
        drawControlCard(t7, L"\xE74F", L"Audio Mute", L"Instant sound killswitch", g_settings.qcShowMute, s_qcMuteToggleRect);

        RECT t8 = {startX + cardW + 16, gridY + rowH * 3, rightEdge, gridY + rowH * 3 + 46};
        drawControlCard(t8, L"\xE83E", L"Battery Status", L"Power level & sleep options", g_settings.qcShowBattery, s_qcBatteryToggleRect);

        // Row 5
        RECT t9 = {startX, gridY + rowH * 4, startX + cardW, gridY + rowH * 4 + 46};
        drawControlCard(t9, L"\xE72E", L"Lock PC", L"Quick lock desktop workstation", g_settings.qcShowLockPC, s_qcLockPCToggleRect);

        // Section 3: Power Mode Selector
        int py = gridY + rowH * 5 + 8;
        drawRowHeader(py, L"Power Plan Pill", L"Show Battery / Balanced / Performance selector.");
        s_qcPowerModeToggleRect = {startX + 280, py + 12, startX + 324, py + 36};
        DrawToggleSwitch(memDC, s_qcPowerModeToggleRect, g_settings.qcShowPowerMode, isDark);

        RECT pPillRc = {rightEdge - 240, py + 8, rightEdge, py + 40};
        std::vector<std::wstring> pOpts = {L"Battery", L"Balanced", L"Performance"};
        DrawSegmentedPills(memDC, pPillRc, pOpts, 2, s_powerPillRects, isDark);
    }
    else if (s_activeTab == STAB_ABOUT)
    {
        int y1 = 44;
        RECT idCardRc = {startX, y1, rightEdge, y1 + 104};
        DrawRoundedRect(memDC, idCardRc, 14, isDark ? RGB(28, 32, 42) : RGB(232, 238, 248), isDark ? RGB(50, 56, 72) : RGB(200, 212, 230), 1);

        HFONT logoBigFont = CreateClearTypeFont(20, FW_BOLD, L"Segoe UI");
        oldF = SelectObject(memDC, logoBigFont);
        SetTextColor(memDC, isDark ? RGB(255, 255, 255) : RGB(16, 20, 28));
        RECT lRc = {startX + 20, y1 + 16, startX + 350, y1 + 42};
        DrawTextW(memDC, L"WinTray", -1, &lRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        SelectObject(memDC, oldF);
        DeleteObject(logoBigFont);

        HFONT vFont = CreateClearTypeFont(12, FW_NORMAL, L"Segoe UI");
        oldF = SelectObject(memDC, vFont);
        SetTextColor(memDC, isDark ? RGB(150, 160, 180) : RGB(90, 100, 115));
        RECT vRc = {startX + 20, y1 + 44, startX + 450, y1 + 62};
        DrawTextW(memDC, L"Version 2.5 - Modern Windows Tray & Productivity Hub", -1, &vRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        RECT cRc = {startX + 20, y1 + 64, startX + 480, y1 + 84};
        DrawTextW(memDC, L"Hardware-accelerated acrylic glassmorphism, dynamic multi-edge dock, and customizable quick controls.", -1, &cRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        SelectObject(memDC, oldF);
        DeleteObject(vFont);

        int y2 = 176;
        s_checkUpdatesBtnRect = {startX, y2, startX + 160, y2 + 38};
        DrawModernActionBtn(memDC, s_checkUpdatesBtnRect, L"Check for Updates", isDark, L"\xE895");

        s_openConfigBtnRect = {startX + 172, y2, startX + 332, y2 + 38};
        DrawModernActionBtn(memDC, s_openConfigBtnRect, L"Open Settings INI", isDark, L"\xE8B7");

        s_resetDefaultsBtnRect = {startX, y2 + 54, startX + 160, y2 + 92};
        DrawModernActionBtn(memDC, s_resetDefaultsBtnRect, L"Reset to Defaults", isDark, L"\xE777");
    }

    DeleteObject(secTitleFont);
    DeleteObject(descFont);

    // Alpha keying with Acrylic Glassmorphism passthrough
    if (pBits)
    {
        DWORD *pixels = static_cast<DWORD *>(pBits);
        int total = width * height;
        if (isDark)
        {
            for (int i = 0; i < total; ++i)
            {
                if (pixels[i] & 0x00FFFFFF)
                {
                    pixels[i] |= 0xFF000000;
                }
                else
                {
                    // Semi-translucent glassmorphism tint over native DWM Acrylic blur
                    int x = i % width;
                    // Mathematically valid premultiplied alpha for dark acrylic glass:
                    pixels[i] = (x < 210) ? 0x48141218 : 0x24100E14;
                }
            }
        }
        else
        {
            // Light Mode:
            // Valid premultiplied alpha for translucent frosted glass in light mode:
            // Sidebar: A = 0x28 (40/255 ~ 16% opacity), RGB(236, 240, 248) -> Premultiplied: R=37, G=38, B=39 (all <= A)
            const DWORD sideGlassLight = (0x28 << 24) | (37 << 16) | (38 << 8) | 39;
            // Main content: 0x00000000 allows native DWM Acrylic with frosted light tint (0x20F0ECE8) to shine through cleanly!
            const DWORD mainGlassLight = 0x00000000;

            for (int i = 0; i < total; ++i)
            {
                int x = i % width;
                DWORD rgb = pixels[i] & 0x00FFFFFF;
                if (x < 210)
                {
                    if (rgb == sideBgDword)
                    {
                        pixels[i] = sideGlassLight;
                    }
                    else
                    {
                        pixels[i] |= 0xFF000000;
                    }
                }
                else
                {
                    if (rgb == mainBgDword)
                    {
                        pixels[i] = mainGlassLight;
                    }
                    else
                    {
                        pixels[i] |= 0xFF000000;
                    }
                }
            }
        }
    }

    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        ApplyWindowBackdrop(hwnd, BackdropType::Acrylic);
        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == s_caretTimerId)
        {
            s_caretVisible = !s_caretVisible;
            InvalidateRect(hwnd, &s_searchBoxRect, FALSE);
            return 0;
        }
        break;
    }

    case WM_NCCALCSIZE:
    {
        if (wParam)
            return 0;
        break;
    }

    case WM_NCHITTEST:
    {
        POINT pt = {(short)LOWORD(lParam), (short)HIWORD(lParam)};
        ScreenToClient(hwnd, &pt);

        if (PtInRect(&s_closeBtnRect, pt))
            return HTCLIENT;

        if (PtInRect(&s_searchBoxRect, pt))
            return HTCLIENT;

        if (pt.y < 44)
            return HTCAPTION;

        if (pt.x < 210 && pt.y < 90)
            return HTCAPTION;

        return HTCLIENT;
    }

    case WM_CHAR:
    {
        if (s_searchFocused)
        {
            if (wParam == VK_BACK)
            {
                if (!s_searchQuery.empty())
                {
                    s_searchQuery.pop_back();
                    PerformSearchFilter(hwnd);
                }
                return 0;
            }
            else if (wParam == VK_ESCAPE)
            {
                s_searchQuery.clear();
                s_searchFocused = false;
                KillTimer(hwnd, s_caretTimerId);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            else if (wParam >= 32 && wParam != 127)
            {
                if (s_searchQuery.size() < 40)
                {
                    s_searchQuery.push_back(static_cast<wchar_t>(wParam));
                    PerformSearchFilter(hwnd);
                }
                return 0;
            }
        }
        break;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        if (s_isRecordingHotkey)
        {
            if (wParam == VK_ESCAPE)
            {
                s_isRecordingHotkey = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (wParam == VK_SHIFT || wParam == VK_CONTROL || wParam == VK_MENU ||
                wParam == VK_LWIN || wParam == VK_RWIN)
            {
                return 0;
            }

            int mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;

            if (mods == 0)
                mods = MOD_WIN | MOD_ALT;

            g_settings.hotkeyMods = mods;
            g_settings.hotkeyVk = static_cast<int>(wParam);
            s_isRecordingHotkey = false;

            SaveSettings();
            if (g_mainHwnd && IsWindow(g_mainHwnd))
            {
                ApplyGlobalHotkey(g_mainHwnd);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt = {(short)LOWORD(lParam), (short)HIWORD(lParam)};
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);

        bool needRedraw = false;
        int newHoverTab = -1;
        for (int i = 0; i < STAB_COUNT; ++i)
        {
            if (PtInRect(&s_tabRects[i], pt))
            {
                newHoverTab = i;
                break;
            }
        }
        if (newHoverTab != s_hoveredTab)
        {
            s_hoveredTab = newHoverTab;
            needRedraw = true;
        }

        bool newCloseHov = PtInRect(&s_closeBtnRect, pt);
        if (newCloseHov != s_closeHovered)
        {
            s_closeHovered = newCloseHov;
            needRedraw = true;
        }

        if (needRedraw)
            InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSELEAVE:
    {
        if (s_hoveredTab != -1 || s_closeHovered)
        {
            s_hoveredTab = -1;
            s_closeHovered = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        POINT pt = {(short)LOWORD(lParam), (short)HIWORD(lParam)};

        // 1. Close button
        if (PtInRect(&s_closeBtnRect, pt))
        {
            SaveSettings();
            DestroyWindow(hwnd);
            return 0;
        }

        // Search clear 'X' button
        if (!s_searchQuery.empty() && PtInRect(&s_searchClearRect, pt))
        {
            s_searchQuery.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Search focus handling
        if (PtInRect(&s_searchBoxRect, pt))
        {
            if (!s_searchFocused)
            {
                s_searchFocused = true;
                s_caretVisible = true;
                SetTimer(hwnd, s_caretTimerId, 500, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        else
        {
            if (s_searchFocused)
            {
                s_searchFocused = false;
                KillTimer(hwnd, s_caretTimerId);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }

        // Cancel hotkey recording if clicked outside hotkey button
        if (s_isRecordingHotkey && !PtInRect(&s_hotkeyBtnRect, pt))
        {
            s_isRecordingHotkey = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        // 2. Sidebar Tabs
        for (int i = 0; i < STAB_COUNT; ++i)
        {
            if (PtInRect(&s_tabRects[i], pt))
            {
                if (s_activeTab != static_cast<SettingsTab>(i))
                {
                    s_activeTab = static_cast<SettingsTab>(i);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }

        auto applyLiveTheme = [&]() {
            SaveSettings();
            UpdateThemeColors();
            ApplyWindowBackdrop(hwnd, BackdropType::Acrylic);
            if (g_mainHwnd && IsWindow(g_mainHwnd))
            {
                ApplyWindowBackdrop(g_mainHwnd, BackdropType::Acrylic);
                SyncTrayOrientation(g_mainHwnd);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        };

        auto applyQuickControlsUpdate = [&]() {
            SaveSettings();
            if (g_mainHwnd && IsWindow(g_mainHwnd))
            {
                SyncTrayOrientation(g_mainHwnd);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        };

        // 3. Tab Specific Interactions
        if (s_activeTab == STAB_APPEARANCE)
        {
            if (PtInRect(&s_previewToggleRect, pt))
            {
                s_previewOpen = !s_previewOpen;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            for (int i = 0; i < 3; ++i)
            {
                if (PtInRect(&s_themePillRects[i], pt))
                {
                    if (g_settings.themeMode != i)
                    {
                        g_settings.themeMode = i;
                        applyLiveTheme();
                    }
                    return 0;
                }
            }

            for (int i = 0; i < 2; ++i)
            {
                if (PtInRect(&s_backdropPillRects[i], pt))
                {
                    if (g_settings.backdropStyle != i)
                    {
                        g_settings.backdropStyle = i;
                        applyLiveTheme();
                    }
                    return 0;
                }
            }

            for (int i = 0; i < 2; ++i)
            {
                if (PtInRect(&s_notchPillRects[i], pt))
                {
                    if (g_settings.notchStyle != i)
                    {
                        g_settings.notchStyle = i;
                        applyLiveTheme();
                    }
                    return 0;
                }
            }

            for (int i = 0; i < 4; ++i)
            {
                if (PtInRect(&s_collapsedPillRects[i], pt))
                {
                    if (g_settings.collapsedStyle != i)
                    {
                        g_settings.collapsedStyle = i;
                        SaveSettings();
                        if (g_mainHwnd && IsWindow(g_mainHwnd))
                            InvalidateRect(g_mainHwnd, nullptr, FALSE);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
            }

            if (PtInRect(&s_largeIconsToggleRect, pt))
            {
                g_settings.largeIcons = !g_settings.largeIcons;
                SaveSettings();
                if (g_mainHwnd && IsWindow(g_mainHwnd))
                {
                    SyncTrayOrientation(g_mainHwnd);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (PtInRect(&s_magnifyToggleRect, pt))
            {
                g_settings.enableMagnify = !g_settings.enableMagnify;
                SaveSettings();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        else if (s_activeTab == STAB_PLAYER)
        {
            if (PtInRect(&s_playerShowMusicTabToggleRect, pt))
            {
                g_settings.showMusicTab = !g_settings.showMusicTab;
                SaveSettings();
                if (g_mainHwnd && IsWindow(g_mainHwnd))
                    InvalidateRect(g_mainHwnd, nullptr, FALSE);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&s_playerShowArtToggleRect, pt))
            {
                g_settings.playerShowCollapsedArt = !g_settings.playerShowCollapsedArt;
                SaveSettings();
                if (g_mainHwnd && IsWindow(g_mainHwnd))
                    InvalidateRect(g_mainHwnd, nullptr, FALSE);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&s_playerShowVisualizerToggleRect, pt))
            {
                g_settings.playerShowVisualizer = !g_settings.playerShowVisualizer;
                SaveSettings();
                if (g_mainHwnd && IsWindow(g_mainHwnd))
                    InvalidateRect(g_mainHwnd, nullptr, FALSE);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            for (int i = 0; i < 4; ++i)
            {
                if (PtInRect(&s_playerColorPillRects[i], pt))
                {
                    g_settings.playerVisualizerColor = i;
                    SaveSettings();
                    if (g_mainHwnd && IsWindow(g_mainHwnd))
                        InvalidateRect(g_mainHwnd, nullptr, FALSE);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (PtInRect(&s_playerResetFavBtnRect, pt))
            {
                InitDefaultFavorites();
                SaveSettings();
                if (g_mainHwnd && IsWindow(g_mainHwnd))
                    InvalidateRect(g_mainHwnd, nullptr, FALSE);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        else if (s_activeTab == STAB_GENERAL)
        {
            if (PtInRect(&s_autostartToggleRect, pt))
            {
                SetAutoStart(!IsAutoStartEnabled());
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (PtInRect(&s_alwaysOnTopToggleRect, pt))
            {
                g_settings.alwaysOnTop = !g_settings.alwaysOnTop;
                SaveSettings();
                if (g_mainHwnd && IsWindow(g_mainHwnd))
                {
                    SetWindowPos(g_mainHwnd, g_settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (PtInRect(&s_autoremoveToggleRect, pt))
            {
                g_settings.autoRemoveUnpinned = !g_settings.autoRemoveUnpinned;
                SaveSettings();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (PtInRect(&s_movefilesToggleRect, pt))
            {
                g_settings.moveFiles = !g_settings.moveFiles;
                SaveSettings();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            // Click to edit Hotkey
            if (PtInRect(&s_hotkeyBtnRect, pt))
            {
                s_isRecordingHotkey = !s_isRecordingHotkey;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        else if (s_activeTab == STAB_ACTIONS)
        {
            if (PtInRect(&s_actRecAudioToggleRect, pt))
            {
                g_settings.recIncludeAudio = !g_settings.recIncludeAudio;
                SaveSettings();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&s_actAutoDropToggleRect, pt))
            {
                g_settings.recAutoDropInTray = !g_settings.recAutoDropInTray;
                SaveSettings();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&s_actTakeShotBtnRect, pt))
            {
                CaptureInstantScreenshot(hwnd);
                return 0;
            }
            if (PtInRect(&s_actSnipBtnRect, pt))
            {
                LaunchRegionSnippingTool();
                return 0;
            }
            if (PtInRect(&s_actOpenCapturesBtnRect, pt))
            {
                wchar_t *pVideos = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &pVideos)))
                {
                    std::wstring dir = std::wstring(pVideos) + L"\\Captures";
                    CoTaskMemFree(pVideos);
                    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                return 0;
            }
            if (PtInRect(&s_actOpenShotsBtnRect, pt))
            {
                wchar_t *pPictures = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pPictures)))
                {
                    std::wstring dir = std::wstring(pPictures) + L"\\Screenshots";
                    CoTaskMemFree(pPictures);
                    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                return 0;
            }
        }
        else if (s_activeTab == STAB_TRAY)
        {
            for (int i = 0; i < 3; ++i)
            {
                if (PtInRect(&s_edgePillRects[i], pt))
                {
                    if (g_settings.dockEdge != i)
                    {
                        g_settings.dockEdge = i;
                        SaveSettings();
                        if (g_mainHwnd && IsWindow(g_mainHwnd))
                        {
                            SyncTrayOrientation(g_mainHwnd);
                        }
                        applyLiveTheme();
                    }
                    return 0;
                }
            }

            for (int i = 0; i < 2; ++i)
            {
                if (PtInRect(&s_notchPillRects[i], pt))
                {
                    if (g_settings.notchStyle != i)
                    {
                        g_settings.notchStyle = i;
                        applyLiveTheme();
                    }
                    return 0;
                }
            }

            for (int i = 0; i < 4; ++i)
            {
                if (PtInRect(&s_collapsedPillRects[i], pt))
                {
                    if (g_settings.collapsedStyle != i)
                    {
                        g_settings.collapsedStyle = i;
                        SaveSettings();
                        if (g_mainHwnd && IsWindow(g_mainHwnd))
                            InvalidateRect(g_mainHwnd, nullptr, FALSE);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
            }

            if (PtInRect(&s_centerTrayBtnRect, pt))
            {
                g_settings.dockOffsetRatio = 0.5f;
                SaveSettings();
                if (g_mainHwnd && IsWindow(g_mainHwnd))
                {
                    SyncTrayOrientation(g_mainHwnd);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        else if (s_activeTab == STAB_CONTROLS)
        {
            if (PtInRect(&s_qcBrightnessToggleRect, pt))
            {
                g_settings.qcShowBrightness = !g_settings.qcShowBrightness;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcVolumeToggleRect, pt))
            {
                g_settings.qcShowVolume = !g_settings.qcShowVolume;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcWifiToggleRect, pt))
            {
                g_settings.qcShowWifi = !g_settings.qcShowWifi;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcBtToggleRect, pt))
            {
                g_settings.qcShowBluetooth = !g_settings.qcShowBluetooth;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcAirplaneToggleRect, pt))
            {
                g_settings.qcShowAirplane = !g_settings.qcShowAirplane;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcHotspotToggleRect, pt))
            {
                g_settings.qcShowHotspot = !g_settings.qcShowHotspot;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcNightLightToggleRect, pt))
            {
                g_settings.qcShowNightLight = !g_settings.qcShowNightLight;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcFocusToggleRect, pt))
            {
                g_settings.qcShowFocus = !g_settings.qcShowFocus;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcMuteToggleRect, pt))
            {
                g_settings.qcShowMute = !g_settings.qcShowMute;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcBatteryToggleRect, pt))
            {
                g_settings.qcShowBattery = !g_settings.qcShowBattery;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcLockPCToggleRect, pt))
            {
                g_settings.qcShowLockPC = !g_settings.qcShowLockPC;
                applyQuickControlsUpdate();
                return 0;
            }
            if (PtInRect(&s_qcPowerModeToggleRect, pt))
            {
                g_settings.qcShowPowerMode = !g_settings.qcShowPowerMode;
                applyQuickControlsUpdate();
                return 0;
            }
            for (int i = 0; i < 3; ++i)
            {
                if (PtInRect(&s_powerPillRects[i], pt))
                {
                    SetPowerMode(static_cast<PowerMode>(i));
                    applyQuickControlsUpdate();
                    return 0;
                }
            }
        }
        else if (s_activeTab == STAB_ABOUT)
        {
            if (PtInRect(&s_checkUpdatesBtnRect, pt))
            {
                CheckForUpdates(hwnd);
                return 0;
            }

            if (PtInRect(&s_openConfigBtnRect, pt))
            {
                wchar_t appData[MAX_PATH];
                if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
                {
                    std::wstring dir = std::wstring(appData) + L"\\WinTray";
                    std::wstring ini = dir + L"\\WinTray.ini";
                    ShellExecuteW(nullptr, L"open", L"notepad.exe", ini.c_str(), nullptr, SW_SHOW);
                }
                return 0;
            }

            if (PtInRect(&s_resetDefaultsBtnRect, pt))
            {
                if (MessageBoxW(hwnd, L"Reset all WinTray settings to defaults?", L"Reset Defaults", MB_YESNO | MB_ICONQUESTION) == IDYES)
                {
                    g_settings = AppSettings{};
                    applyLiveTheme();
                }
                return 0;
            }
        }

        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintSettingsWindow(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OpenSettingsDialog(HWND parentHwnd)
{
    const wchar_t CLASS_NAME[] = L"WinTrayModernSettingsWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int dlgW = 820;
    int dlgH = 580;

    int posX = (screenW - dlgW) / 2;
    int posY = (screenH - dlgH) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_TOPMOST,
        CLASS_NAME,
        L"WinTray Settings",
        WS_POPUP | WS_THICKFRAME | WS_VISIBLE,
        posX, posY, dlgW, dlgH,
        parentHwnd,
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    EnableWindow(parentHwnd, FALSE);
    MSG msg = {};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(parentHwnd, TRUE);
    SetForegroundWindow(parentHwnd);
}
