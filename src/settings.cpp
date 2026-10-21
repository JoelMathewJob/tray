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
constexpr int ID_CB_HOTKEY_KEY = 2008;
constexpr int ID_CHK_MOD_CTRL = 2009;
constexpr int ID_CHK_MOD_ALT = 2010;
constexpr int ID_CHK_MOD_SHIFT = 2011;
constexpr int ID_CHK_MOD_WIN = 2012;
constexpr int ID_CB_EDGE = 2013;

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
    if (g_iniPath.empty()) return;
    g_settings.moveFiles = GetPrivateProfileIntW(L"Preferences", L"MoveFiles", 0, g_iniPath.c_str()) == 1;
    g_settings.autoRemoveUnpinned = GetPrivateProfileIntW(L"Preferences", L"AutoRemoveUnpinned", 1, g_iniPath.c_str()) == 1;
    g_settings.enableMagnify = GetPrivateProfileIntW(L"Accessibility", L"EnableMagnify", 1, g_iniPath.c_str()) == 1;
    g_settings.largeIcons = GetPrivateProfileIntW(L"Accessibility", L"LargeIcons", 0, g_iniPath.c_str()) == 1;
    g_settings.hotkeyMods = GetPrivateProfileIntW(L"Hotkeys", L"Mods", MOD_WIN | MOD_ALT, g_iniPath.c_str());
    g_settings.hotkeyVk = GetPrivateProfileIntW(L"Hotkeys", L"Vk", 'T', g_iniPath.c_str());
    g_settings.dockEdge = GetPrivateProfileIntW(L"Window", L"DockEdge", EDGE_TOP, g_iniPath.c_str());
}

void SaveSettings()
{
    if (g_iniPath.empty()) return;
    WritePrivateProfileStringW(L"Preferences", L"MoveFiles", g_settings.moveFiles ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Preferences", L"AutoRemoveUnpinned", g_settings.autoRemoveUnpinned ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Accessibility", L"EnableMagnify", g_settings.enableMagnify ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Accessibility", L"LargeIcons", g_settings.largeIcons ? L"1" : L"0", g_iniPath.c_str());
    
    wchar_t buf[32];
    swprintf_s(buf, L"%d", g_settings.hotkeyMods);
    WritePrivateProfileStringW(L"Hotkeys", L"Mods", buf, g_iniPath.c_str());
    swprintf_s(buf, L"%d", g_settings.hotkeyVk);
    WritePrivateProfileStringW(L"Hotkeys", L"Vk", buf, g_iniPath.c_str());
    swprintf_s(buf, L"%d", g_settings.dockEdge);
    WritePrivateProfileStringW(L"Window", L"DockEdge", buf, g_iniPath.c_str());
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
            RegSetValueExW(hKey, L"WinTray", 0, REG_SZ, reinterpret_cast<const BYTE *>(exePath), static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
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
        HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HWND hGroup0 = CreateWindowW(L"BUTTON", L" Screen Dock Edge ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 18, 14, 340, 65, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hGroup0, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hCbEdge = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 32, 40, 310, 120, hwnd, (HMENU)(UINT_PTR)ID_CB_EDGE, nullptr, nullptr);
        SendMessageW(hCbEdge, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCbEdge, CB_ADDSTRING, 0, (LPARAM)L"Top Edge");
        SendMessageW(hCbEdge, CB_ADDSTRING, 0, (LPARAM)L"Right Edge");
        SendMessageW(hCbEdge, CB_ADDSTRING, 0, (LPARAM)L"Left Edge");
        SendMessageW(hCbEdge, CB_SETCURSEL, g_settings.dockEdge, 0);

        HWND hGroup1 = CreateWindowW(L"BUTTON", L" File Dragging & Dropping ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 18, 85, 340, 90, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hGroup1, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hChkMove = CreateWindowW(L"BUTTON", L"Move files instead of Copying when dragging out", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 32, 110, 310, 22, hwnd, (HMENU)(UINT_PTR)ID_CHK_MOVE, nullptr, nullptr);
        SendMessageW(hChkMove, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkMove, BM_SETCHECK, g_settings.moveFiles ? BST_CHECKED : BST_UNCHECKED, 0);

        // HWND hChkRemove = CreateWindowW(L"BUTTON", L"Automatically remove files after drag-out", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 32, 138, 310, 22, hwnd, (HMENU)(UINT_PTR)ID_CHK_AUTOREMOVE, nullptr, nullptr);
        // SendMessageW(hChkRemove, WM_SETFONT, (WPARAM)hFont, TRUE);
        // SendMessageW(hChkRemove, BM_SETCHECK, g_settings.autoRemoveUnpinned ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hGroup2 = CreateWindowW(L"BUTTON", L" Global Toggle Shortcut ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 18, 182, 340, 75, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hGroup2, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hChkCtrl = CreateWindowW(L"BUTTON", L"Ctrl", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 32, 205, 55, 22, hwnd, (HMENU)(UINT_PTR)ID_CHK_MOD_CTRL, nullptr, nullptr);
        SendMessageW(hChkCtrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkCtrl, BM_SETCHECK, (g_settings.hotkeyMods & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hChkAlt = CreateWindowW(L"BUTTON", L"Alt", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 95, 205, 55, 22, hwnd, (HMENU)(UINT_PTR)ID_CHK_MOD_ALT, nullptr, nullptr);
        SendMessageW(hChkAlt, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkAlt, BM_SETCHECK, (g_settings.hotkeyMods & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hChkShift = CreateWindowW(L"BUTTON", L"Shift", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 158, 205, 60, 22, hwnd, (HMENU)(UINT_PTR)ID_CHK_MOD_SHIFT, nullptr, nullptr);
        SendMessageW(hChkShift, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkShift, BM_SETCHECK, (g_settings.hotkeyMods & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hChkWin = CreateWindowW(L"BUTTON", L"Win", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 226, 205, 50, 22, hwnd, (HMENU)(UINT_PTR)ID_CHK_MOD_WIN, nullptr, nullptr);
        SendMessageW(hChkWin, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkWin, BM_SETCHECK, (g_settings.hotkeyMods & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hCbKey = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 285, 204, 60, 120, hwnd, (HMENU)(UINT_PTR)ID_CB_HOTKEY_KEY, nullptr, nullptr);
        SendMessageW(hCbKey, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCbKey, CB_ADDSTRING, 0, (LPARAM)L"A");
        SendMessageW(hCbKey, CB_ADDSTRING, 0, (LPARAM)L"T");
        SendMessageW(hCbKey, CB_ADDSTRING, 0, (LPARAM)L"Space");
        SendMessageW(hCbKey, CB_ADDSTRING, 0, (LPARAM)L"Q");
        SendMessageW(hCbKey, CB_ADDSTRING, 0, (LPARAM)L"E");
        if (g_settings.hotkeyVk == VK_SPACE) SendMessageW(hCbKey, CB_SETCURSEL, 2, 0);
        else SendMessageW(hCbKey, CB_SELECTSTRING, -1, (LPARAM)(g_settings.hotkeyVk == 'A' ? L"A" : (g_settings.hotkeyVk == 'T' ? L"T" : (g_settings.hotkeyVk == 'Q' ? L"Q" : L"E"))));

        HWND hGroup3 = CreateWindowW(L"BUTTON", L" Startup & Updates ", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 18, 264, 340, 85, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hGroup3, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hChkStartup = CreateWindowW(L"BUTTON", L"Launch WinTray on Windows Startup", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 32, 288, 310, 22, hwnd, (HMENU)(UINT_PTR)ID_CHK_STARTUP, nullptr, nullptr);
        SendMessageW(hChkStartup, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkStartup, BM_SETCHECK, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

        // Buttons side-by-side on the same Y level (y = 316)
        HWND hBtnUpdate = CreateWindowW(L"BUTTON", L"Check for Updates...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 32, 316, 160, 30, hwnd, (HMENU)(UINT_PTR)ID_BTN_UPDATE, nullptr, nullptr);
        SendMessageW(hBtnUpdate, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnClose = CreateWindowW(L"BUTTON", L"Save & Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 202, 316, 140, 30, hwnd, (HMENU)(UINT_PTR)ID_BTN_CLOSE, nullptr, nullptr);
        SendMessageW(hBtnClose, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == ID_BTN_UPDATE) CheckForUpdates(hwnd);
        else if (id == ID_BTN_CLOSE)
        {
            // CRITICAL FIX: Read checkbox states and update memory BEFORE saving
            g_settings.moveFiles = (IsDlgButtonChecked(hwnd, ID_CHK_MOVE) == BST_CHECKED);
            g_settings.autoRemoveUnpinned = (IsDlgButtonChecked(hwnd, ID_CHK_AUTOREMOVE) == BST_CHECKED);
            
            bool enableStartup = (IsDlgButtonChecked(hwnd, ID_CHK_STARTUP) == BST_CHECKED);
            SetAutoStart(enableStartup);

            HWND hEdgeCb = GetDlgItem(hwnd, ID_CB_EDGE);
            g_settings.dockEdge = static_cast<int>(SendMessageW(hEdgeCb, CB_GETCURSEL, 0, 0));

            int mods = 0;
            if (IsDlgButtonChecked(hwnd, ID_CHK_MOD_CTRL) == BST_CHECKED) mods |= MOD_CONTROL;
            if (IsDlgButtonChecked(hwnd, ID_CHK_MOD_ALT) == BST_CHECKED) mods |= MOD_ALT;
            if (IsDlgButtonChecked(hwnd, ID_CHK_MOD_SHIFT) == BST_CHECKED) mods |= MOD_SHIFT;
            if (IsDlgButtonChecked(hwnd, ID_CHK_MOD_WIN) == BST_CHECKED) mods |= MOD_WIN;
            g_settings.hotkeyMods = mods ? mods : MOD_WIN;

            HWND hCb = GetDlgItem(hwnd, ID_CB_HOTKEY_KEY);
            int sel = static_cast<int>(SendMessageW(hCb, CB_GETCURSEL, 0, 0));
            if (sel == 2) g_settings.hotkeyVk = VK_SPACE;
            else if (sel == 0) g_settings.hotkeyVk = 'A';
            else if (sel == 1) g_settings.hotkeyVk = 'T';
            else if (sel == 3) g_settings.hotkeyVk = 'Q';
            else if (sel == 4) g_settings.hotkeyVk = 'E';

            SaveSettings();
            ApplyGlobalHotkey(g_mainHwnd);
            UpdateTrayWindowGeometry(g_mainHwnd);
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

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int dlgW = 395;
    int dlgH = 410;

    RECT rc;
    GetWindowRect(parentHwnd, &rc);
    
    // Position intelligently so it never opens off-screen
    int posX = rc.left + 50;
    int posY = rc.bottom + 15;
    if (posX + dlgW > screenW) posX = screenW - dlgW - 20;
    if (posX < 0) posX = 20;
    if (posY + dlgH > screenH) posY = rc.top - dlgH - 15;
    if (posY < 0) posY = 20;

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, CLASS_NAME, L"WinTray Settings", WS_VISIBLE | WS_SYSMENU | WS_CAPTION, posX, posY, dlgW, dlgH, parentHwnd, nullptr, GetModuleHandle(nullptr), nullptr);

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