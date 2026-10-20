#include "settings.h"
#include "state.h"
#include "updater.h"
#include <shlobj.h>
#include <string>

static std::wstring g_iniPath;

constexpr int ID_CHK_MOVE = 2001;
constexpr int ID_CHK_AUTOREMOVE = 2002;
constexpr int ID_CHK_MAGNIFY = 2003;
constexpr int ID_CHK_LARGEICONS = 2004;
constexpr int ID_BTN_UPDATE = 2005;
constexpr int ID_BTN_CLOSE = 2006;
constexpr int ID_CHK_STARTUP = 2007;

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
    g_settings.moveFiles = GetPrivateProfileIntW(L"Preferences", L"MoveFiles", 0, g_iniPath.c_str()) == 1;
    g_settings.autoRemoveUnpinned = GetPrivateProfileIntW(L"Preferences", L"AutoRemoveUnpinned", 1, g_iniPath.c_str()) == 1;
    g_settings.enableMagnify = GetPrivateProfileIntW(L"Accessibility", L"EnableMagnify", 1, g_iniPath.c_str()) == 1;
    g_settings.largeIcons = GetPrivateProfileIntW(L"Accessibility", L"LargeIcons", 0, g_iniPath.c_str()) == 1;
}

void SaveSettings()
{
    if (g_iniPath.empty())
        return;
    WritePrivateProfileStringW(L"Preferences", L"MoveFiles", g_settings.moveFiles ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Preferences", L"AutoRemoveUnpinned", g_settings.autoRemoveUnpinned ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Accessibility", L"EnableMagnify", g_settings.enableMagnify ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Accessibility", L"LargeIcons", g_settings.largeIcons ? L"1" : L"0", g_iniPath.c_str());
}

bool IsAutoStartEnabled()
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

void SetAutoStart(bool enable)
{
    HKEY hKey;
    const wchar_t *subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (enable)
        {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            RegSetValueExW(hKey, L"WinTray", 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(exePath),
                           static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(hKey, L"WinTray");
        }
        RegCloseKey(hKey);
    }
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HWND hGroup1 = CreateWindowW(L"BUTTON", L" File Dragging & Dropping ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                     18, 14, 340, 95, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hGroup1, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hChkMove = CreateWindowW(L"BUTTON", L"Move files instead of Copying when dragging out",
                                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      32, 40, 310, 24, hwnd, (HMENU)(UINT_PTR)ID_CHK_MOVE, nullptr, nullptr);
        SendMessageW(hChkMove, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkMove, BM_SETCHECK, g_settings.moveFiles ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hChkRemove = CreateWindowW(L"BUTTON", L"Automatically remove files after drag-out (if unpinned)",
                                        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        32, 70, 310, 24, hwnd, (HMENU)(UINT_PTR)ID_CHK_AUTOREMOVE, nullptr, nullptr);
        SendMessageW(hChkRemove, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkRemove, BM_SETCHECK, g_settings.autoRemoveUnpinned ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hGroup2 = CreateWindowW(L"BUTTON", L" Accessibility & Visuals ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                     18, 120, 340, 125, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hGroup2, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hChkMag = CreateWindowW(L"BUTTON", L"Enable smooth magnification on hover",
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     32, 145, 310, 24, hwnd, (HMENU)(UINT_PTR)ID_CHK_MAGNIFY, nullptr, nullptr);
        SendMessageW(hChkMag, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkMag, BM_SETCHECK, g_settings.enableMagnify ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hChkLarge = CreateWindowW(L"BUTTON", L"Enlarged tray size and high-visibility icons",
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       32, 175, 310, 24, hwnd, (HMENU)(UINT_PTR)ID_CHK_LARGEICONS, nullptr, nullptr);
        SendMessageW(hChkLarge, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkLarge, BM_SETCHECK, g_settings.largeIcons ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hChkStartup = CreateWindowW(
            L"BUTTON", L"Launch WinTray on Windows Startup",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            32, 205, 310, 24, hwnd, (HMENU)(UINT_PTR)ID_CHK_STARTUP, nullptr, nullptr);
        SendMessageW(hChkStartup, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkStartup, BM_SETCHECK, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hBtnUpdate = CreateWindowW(L"BUTTON", L"Check for Updates...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        18, 255, 160, 32, hwnd, (HMENU)(UINT_PTR)ID_BTN_UPDATE, nullptr, nullptr);
        SendMessageW(hBtnUpdate, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnClose = CreateWindowW(L"BUTTON", L"Save & Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                       248, 255, 110, 32, hwnd, (HMENU)(UINT_PTR)ID_BTN_CLOSE, nullptr, nullptr);
        SendMessageW(hBtnClose, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == ID_CHK_MOVE)
            g_settings.moveFiles = (IsDlgButtonChecked(hwnd, ID_CHK_MOVE) == BST_CHECKED);
        else if (id == ID_CHK_AUTOREMOVE)
            g_settings.autoRemoveUnpinned = (IsDlgButtonChecked(hwnd, ID_CHK_AUTOREMOVE) == BST_CHECKED);
        else if (id == ID_CHK_MAGNIFY)
            g_settings.enableMagnify = (IsDlgButtonChecked(hwnd, ID_CHK_MAGNIFY) == BST_CHECKED);
        else if (id == ID_CHK_LARGEICONS)
            g_settings.largeIcons = (IsDlgButtonChecked(hwnd, ID_CHK_LARGEICONS) == BST_CHECKED);
        else if (id == ID_CHK_STARTUP)
            SetAutoStart(IsDlgButtonChecked(hwnd, ID_CHK_STARTUP) == BST_CHECKED);
        else if (id == ID_BTN_UPDATE)
            CheckForUpdates(hwnd);
        else if (id == ID_BTN_CLOSE)
        {
            SaveSettings();
            DestroyWindow(hwnd);
        }
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
    const wchar_t CLASS_NAME[] = L"WinTraySettingsWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    RECT rc;
    GetWindowRect(parentHwnd, &rc);

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        CLASS_NAME,
        L"WinTray Settings",
        WS_VISIBLE | WS_SYSMENU | WS_CAPTION,
        rc.left - 50, rc.bottom + 15, 395, 345,
        parentHwnd, nullptr, GetModuleHandle(nullptr), nullptr);

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