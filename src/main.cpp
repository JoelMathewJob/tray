#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wininet.h>
#include <commctrl.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include "blur.h"
#include "version_info.h"

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "Comctl32.lib")


// User Preferences Struct
struct AppSettings
{
    bool moveFiles = false;         // false = Copy (Default), true = Move
    bool autoRemoveUnpinned = true; // Remove after drop-out
    bool enableMagnify = true;      // Accessibility: Hover magnification
    bool largeIcons = false;        // Accessibility: Larger scale
};

static AppSettings g_settings;
static std::wstring g_iniPath;

// Dynamic Dimensions
constexpr float COLLAPSED_W = 220.0f;
constexpr float COLLAPSED_H = 16.0f;
constexpr int CORNER_RADIUS = 28;

struct PinnedItem
{
    std::wstring path;
    std::wstring name;
    HICON icon = nullptr;
    RECT rect = {};
    bool isSelected = false;
    bool isPinned = false;
    float currentScale = 1.0f;
};

static std::vector<PinnedItem> g_items;
static int g_hoveredIndex = -1;
static int g_lastClickedIndex = -1;
static POINT g_dragStartPoint = {};
static bool g_isTrackingDrag = false;
static int g_dragCandidateIndex = -1;
static bool g_isSelfDragging = false;
static bool g_isMenuOpen = false;

// Settings Button rect on tray
static RECT g_settingsBtnRect = {};
static bool g_settingsHovered = false;

// Animation & scrolling state
static bool g_isExpanded = false;
static bool g_mouseTrackingActive = false;
static float g_curW = COLLAPSED_W;
static float g_curH = COLLAPSED_H;
static float g_targetW = COLLAPSED_W;
static float g_targetH = COLLAPSED_H;

static float g_scrollOffset = 0.0f;
static float g_targetScroll = 0.0f;
static float g_maxScroll = 0.0f;

constexpr UINT_PTR ANIM_TIMER_ID = 42;

// -------------------------------------------------------------
// Settings INI Persistence
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// GitHub Release Updater
// -------------------------------------------------------------
// Helper function to download a remote binary to disk using WinINet
bool DownloadBinaryToDisk(const std::wstring &url, const std::wstring &destPath)
{
    HINTERNET hInternet = InternetOpenW(L"WinTrayUpdater", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet)
        return false;

    // Follow redirects automatically to handle GitHub asset CDN routing
    HINTERNET hFile = InternetOpenUrlW(
        hInternet, url.c_str(),
        L"User-Agent: WinTrayApp\r\n", -1,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);

    if (!hFile)
    {
        InternetCloseHandle(hInternet);
        return false;
    }

    HANDLE hOut = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE)
    {
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    char buffer[8192];
    DWORD bytesRead = 0;
    DWORD bytesWritten = 0;
    bool success = true;

    while (InternetReadFile(hFile, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0)
    {
        if (!WriteFile(hOut, buffer, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead)
        {
            success = false;
            break;
        }
    }

    CloseHandle(hOut);
    InternetCloseHandle(hFile);
    InternetCloseHandle(hInternet);

    if (!success)
    {
        DeleteFileW(destPath.c_str());
    }
    return success;
}

// Executes a detached cmd script that waits for current process exit, overwrites it, and relaunches
void ExecuteInPlaceUpdate(const std::wstring &newExePath)
{
    wchar_t currentExe[MAX_PATH];
    GetModuleFileNameW(nullptr, currentExe, MAX_PATH);

    std::wstring batchCmd =
        L"/c timeout /t 1 /nobreak > NUL & "
        L"move /y \"" +
        newExePath + L"\" \"" + currentExe + L"\" & "
                                             L"start \"\" \"" +
        currentExe + L"\"";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = batchCmd.c_str();
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei))
    {
        ExitProcess(0); // Terminate current instance immediately to release the file lock
    }
}

// Checks GitHub releases and triggers download & in-place restart
void CheckForUpdates(HWND parentHwnd)
{
    HINTERNET hInternet = InternetOpenW(L"WinTrayUpdater", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet)
    {
        MessageBoxW(parentHwnd, L"Unable to access network to check for updates.", L"Update Check", MB_ICONWARNING);
        return;
    }

    std::wstring url = L"https://api.github.com/repos/" + GITHUB_REPO + L"/releases/latest";
    HINTERNET hFile = InternetOpenUrlW(
        hInternet, url.c_str(),
        L"User-Agent: WinTrayApp\r\n", -1,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);

    if (!hFile)
    {
        InternetCloseHandle(hInternet);
        MessageBoxW(parentHwnd, L"Could not query release server or repository does not exist.", L"Update Check", MB_ICONINFORMATION);
        return;
    }

    char buffer[4096] = {};
    DWORD bytesRead = 0;
    std::string response = "";
    while (InternetReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0)
    {
        buffer[bytesRead] = '\0';
        response += buffer;
    }
    InternetCloseHandle(hFile);
    InternetCloseHandle(hInternet);

    // 1. Parse tag_name
    std::string tagKey = "\"tag_name\":\"";
    size_t tagPos = response.find(tagKey);
    if (tagPos == std::string::npos)
    {
        MessageBoxW(parentHwnd, L"No release information found.", L"Update Check", MB_ICONINFORMATION);
        return;
    }

    size_t startTag = tagPos + tagKey.length();
    size_t endTag = response.find("\"", startTag);
    std::string latestTag = response.substr(startTag, endTag - startTag);
    std::wstring wLatestTag(latestTag.begin(), latestTag.end());

    if (wLatestTag == CURRENT_VERSION)
    {
        std::wstring upToDateMsg = L"You are running the latest version of WinTray (" + CURRENT_VERSION + L").";
        MessageBoxW(parentHwnd, upToDateMsg.c_str(), L"WinTray is Up to Date", MB_ICONINFORMATION);
        return;
    }

    // 2. Parse browser_download_url for WinTray.exe
    std::string downloadUrl = "";
    std::string urlKey = "\"browser_download_url\":\"";
    size_t searchPos = 0;
    while ((searchPos = response.find(urlKey, searchPos)) != std::string::npos)
    {
        size_t uStart = searchPos + urlKey.length();
        size_t uEnd = response.find("\"", uStart);
        std::string candidateUrl = response.substr(uStart, uEnd - uStart);
        if (candidateUrl.rfind("WinTray.exe") != std::string::npos)
        {
            downloadUrl = candidateUrl;
            break;
        }
        searchPos = uEnd;
    }

    if (downloadUrl.empty())
    {
        MessageBoxW(parentHwnd, L"New version found, but WinTray.exe was not attached to the release.", L"Update Error", MB_ICONWARNING);
        return;
    }

    std::wstring prompt = L"A new version (" + wLatestTag + L") is available!\nCurrent version: " + CURRENT_VERSION + L"\n\nWould you like to download and install it now?";
    if (MessageBoxW(parentHwnd, prompt.c_str(), L"Update Available", MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        return;
    }

    // 3. Download to %TEMP%
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring tempExePath = std::wstring(tempDir) + L"WinTray_Update.exe";

    std::wstring wDownloadUrl(downloadUrl.begin(), downloadUrl.end());
    if (!DownloadBinaryToDisk(wDownloadUrl, tempExePath))
    {
        MessageBoxW(parentHwnd, L"Failed to download the update package.", L"Update Failed", MB_ICONERROR);
        return;
    }

    // 4. Overwrite running executable and restart
    ExecuteInPlaceUpdate(tempExePath);
}
// -------------------------------------------------------------
// Settings Dialog Window
// -------------------------------------------------------------
constexpr int ID_CHK_MOVE = 2001;
constexpr int ID_CHK_AUTOREMOVE = 2002;
constexpr int ID_CHK_MAGNIFY = 2003;
constexpr int ID_CHK_LARGEICONS = 2004;
constexpr int ID_BTN_UPDATE = 2005;
constexpr int ID_BTN_CLOSE = 2006;
constexpr int ID_CHK_STARTUP = 2007;

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

        HWND hChkStartup = CreateWindowW(
            L"BUTTON", L"Launch WinTray on Windows Startup",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            32, 205, 310, 24, hwnd, (HMENU)(UINT_PTR)ID_CHK_STARTUP, nullptr, nullptr);
        SendMessageW(hChkStartup, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hChkStartup, BM_SETCHECK, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

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

        // Move Update button down to y = 255:
        HWND hBtnUpdate = CreateWindowW(L"BUTTON", L"Check for Updates...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        18, 255, 160, 32, hwnd, (HMENU)(UINT_PTR)ID_BTN_UPDATE, nullptr, nullptr);

        // Move Close button down to y = 255:
        HWND hBtnClose = CreateWindowW(L"BUTTON", L"Save & Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                       248, 255, 110, 32, hwnd, (HMENU)(UINT_PTR)ID_BTN_CLOSE, nullptr, nullptr);
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == ID_CHK_MOVE)
        {
            g_settings.moveFiles = (IsDlgButtonChecked(hwnd, ID_CHK_MOVE) == BST_CHECKED);
        }
        else if (id == ID_CHK_AUTOREMOVE)
        {
            g_settings.autoRemoveUnpinned = (IsDlgButtonChecked(hwnd, ID_CHK_AUTOREMOVE) == BST_CHECKED);
        }
        else if (id == ID_CHK_MAGNIFY)
        {
            g_settings.enableMagnify = (IsDlgButtonChecked(hwnd, ID_CHK_MAGNIFY) == BST_CHECKED);
        }
        else if (id == ID_CHK_LARGEICONS)
        {
            g_settings.largeIcons = (IsDlgButtonChecked(hwnd, ID_CHK_LARGEICONS) == BST_CHECKED);
        }
        else if (id == ID_BTN_UPDATE)
        {
            CheckForUpdates(hwnd);
        }
        else if (id == ID_BTN_CLOSE)
        {
            SaveSettings();
            DestroyWindow(hwnd);
        }
        else if (id == ID_CHK_STARTUP)
        {
            bool shouldEnable = (IsDlgButtonChecked(hwnd, ID_CHK_STARTUP) == BST_CHECKED);
            SetAutoStart(shouldEnable);
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

// -------------------------------------------------------------
// OLE Formats & Enumeration
// -------------------------------------------------------------
class SimpleEnumFORMATETC : public IEnumFORMATETC
{
    LONG m_cRef = 1;
    std::vector<FORMATETC> m_formats;
    size_t m_index = 0;

public:
    SimpleEnumFORMATETC(const std::vector<FORMATETC> &formats) : m_formats(formats) {}
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC)
        {
            *ppv = static_cast<IEnumFORMATETC *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG)
    AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG)
    Release() override
    {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0)
            delete this;
        return c;
    }
    STDMETHODIMP Next(ULONG celt, FORMATETC *rgelt, ULONG *pceltFetched) override
    {
        ULONG fetched = 0;
        while (m_index < m_formats.size() && fetched < celt)
        {
            rgelt[fetched] = m_formats[m_index++];
            fetched++;
        }
        if (pceltFetched)
            *pceltFetched = fetched;
        return (fetched == celt) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG celt) override
    {
        m_index = (std::min)(m_index + celt, m_formats.size());
        return (m_index < m_formats.size()) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override
    {
        m_index = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumFORMATETC **ppenum) override
    {
        if (!ppenum)
            return E_POINTER;
        *ppenum = new SimpleEnumFORMATETC(m_formats);
        return S_OK;
    }
};

class SimpleDropSource : public IDropSource
{
    LONG m_cRef = 1;

public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropSource)
        {
            *ppv = static_cast<IDropSource *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG)
    AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG)
    Release() override
    {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0)
            delete this;
        return c;
    }
    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override
    {
        if (fEscapePressed)
            return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & MK_LBUTTON))
            return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

class MultiFileDataObject : public IDataObject
{
    LONG m_cRef = 1;
    std::vector<std::wstring> m_paths;
    UINT m_dropEffectFormat;
    DWORD m_effectToUse;

public:
    MultiFileDataObject(const std::vector<std::wstring> &paths, DWORD effect)
        : m_paths(paths), m_effectToUse(effect)
    {
        m_dropEffectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDataObject)
        {
            *ppv = static_cast<IDataObject *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG)
    AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG)
    Release() override
    {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0)
            delete this;
        return c;
    }
    STDMETHODIMP GetData(FORMATETC *pFormatetc, STGMEDIUM *pmedium) override
    {
        if (!pFormatetc || !pmedium)
            return E_POINTER;

        if (pFormatetc->cfFormat == CF_HDROP && (pFormatetc->tymed & TYMED_HGLOBAL))
        {
            SIZE_T strBytes = 0;
            for (const auto &p : m_paths)
                strBytes += (p.length() + 1) * sizeof(wchar_t);
            strBytes += sizeof(wchar_t);

            SIZE_T totalBytes = sizeof(DROPFILES) + strBytes;
            HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, totalBytes);
            if (!hGlobal)
                return E_OUTOFMEMORY;

            auto *df = static_cast<DROPFILES *>(GlobalLock(hGlobal));
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;

            wchar_t *dest = reinterpret_cast<wchar_t *>(reinterpret_cast<BYTE *>(df) + sizeof(DROPFILES));
            for (const auto &p : m_paths)
            {
                wcscpy_s(dest, p.length() + 1, p.c_str());
                dest += p.length() + 1;
            }
            *dest = L'\0';
            GlobalUnlock(hGlobal);

            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hGlobal;
            pmedium->pUnkForRelease = nullptr;
            return S_OK;
        }

        if (pFormatetc->cfFormat == m_dropEffectFormat && (pFormatetc->tymed & TYMED_HGLOBAL))
        {
            HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DWORD));
            if (!hGlobal)
                return E_OUTOFMEMORY;
            auto *pEffect = static_cast<DWORD *>(GlobalLock(hGlobal));
            *pEffect = m_effectToUse;
            GlobalUnlock(hGlobal);

            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hGlobal;
            pmedium->pUnkForRelease = nullptr;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    STDMETHODIMP QueryGetData(FORMATETC *pFormatetc) override
    {
        if (!pFormatetc)
            return E_POINTER;
        if (pFormatetc->cfFormat == CF_HDROP && (pFormatetc->tymed & TYMED_HGLOBAL))
            return S_OK;
        if (pFormatetc->cfFormat == m_dropEffectFormat && (pFormatetc->tymed & TYMED_HGLOBAL))
            return S_OK;
        return DV_E_FORMATETC;
    }
    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppenumFormatEtc) override
    {
        if (!ppenumFormatEtc)
            return E_POINTER;
        if (dwDirection == DATADIR_GET)
        {
            std::vector<FORMATETC> formats = {
                {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                {(CLIPFORMAT)m_dropEffectFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
            *ppenumFormatEtc = new SimpleEnumFORMATETC(formats);
            return S_OK;
        }
        return E_NOTIMPL;
    }
    STDMETHODIMP GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC *, FORMATETC *) override { return E_NOTIMPL; }
    STDMETHODIMP SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return E_NOTIMPL; }
    STDMETHODIMP DUnadvise(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA **) override { return E_NOTIMPL; }
};

// -------------------------------------------------------------
// Layout & Section Calculation
// -------------------------------------------------------------
void CalculateTargetSize(bool expanded)
{
    if (expanded)
    {
        int pinnedCount = 0;
        int unpinnedCount = 0;
        for (const auto &item : g_items)
        {
            if (item.isPinned)
                pinnedCount++;
            else
                unpinnedCount++;
        }

        int itemWidth = g_settings.largeIcons ? 74 : 64;
        int pinnedW = pinnedCount * itemWidth;
        int unpinnedW = unpinnedCount * itemWidth;
        int basePadding = 90 + ((pinnedCount > 0 && unpinnedCount > 0) ? 30 : 0);

        int contentWidth = (std::max)(460, pinnedW + unpinnedW + basePadding);
        g_targetW = static_cast<float>((std::min)(contentWidth, 980));
        g_targetH = g_settings.largeIcons ? 136.0f : 124.0f;
    }
    else
    {
        g_targetW = COLLAPSED_W;
        g_targetH = COLLAPSED_H;
    }
}

// -------------------------------------------------------------
// Missing Items Auto-Purge
// -------------------------------------------------------------
void PruneMissingItems(HWND hwnd)
{
    bool changed = false;
    auto it = g_items.begin();
    while (it != g_items.end())
    {
        DWORD attrs = GetFileAttributesW(it->path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            if (it->icon)
                DestroyIcon(it->icon);
            it = g_items.erase(it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }

    if (changed)
    {
        g_hoveredIndex = -1;
        g_lastClickedIndex = -1;
        CalculateTargetSize(g_isExpanded);
        SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

void AddFileToTray(const std::wstring &filePath, bool isPinned = false)
{
    for (const auto &item : g_items)
    {
        if (_wcsicmp(item.path.c_str(), filePath.c_str()) == 0)
            return;
    }

    SHFILEINFOW sfi = {};
    SHGetFileInfoW(filePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_DISPLAYNAME);

    PinnedItem item;
    item.path = filePath;
    item.name = sfi.szDisplayName;
    item.icon = sfi.hIcon;
    item.isSelected = false;
    item.isPinned = isPinned;
    item.currentScale = 1.0f;
    g_items.push_back(item);
}

void CompressSelectedToZip(HWND hwnd)
{
    std::vector<std::wstring> selectedPaths;
    for (const auto &item : g_items)
    {
        if (item.isSelected)
            selectedPaths.push_back(item.path);
    }
    if (selectedPaths.empty())
        return;

    std::wstring chosenZipPath = L"";
    IFileSaveDialog *pFileSave = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_IFileSaveDialog, reinterpret_cast<void **>(&pFileSave));

    if (SUCCEEDED(hr))
    {
        COMDLG_FILTERSPEC fileTypes[] = {{L"ZIP Archive (*.zip)", L"*.zip"}};
        pFileSave->SetFileTypes(1, fileTypes);
        pFileSave->SetDefaultExtension(L"zip");
        pFileSave->SetFileName(L"tray_archive.zip");
        pFileSave->SetTitle(L"Save Compressed Tray Archive");

        PIDLIST_ABSOLUTE pidlDesktop = nullptr;
        if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOPDIRECTORY, &pidlDesktop)))
        {
            IShellItem *pDesktopItem = nullptr;
            if (SUCCEEDED(SHCreateItemFromIDList(pidlDesktop, IID_IShellItem, reinterpret_cast<void **>(&pDesktopItem))))
            {
                pFileSave->SetFolder(pDesktopItem);
                pDesktopItem->Release();
            }
            CoTaskMemFree(pidlDesktop);
        }

        if (SUCCEEDED(pFileSave->Show(hwnd)))
        {
            IShellItem *pResultItem = nullptr;
            if (SUCCEEDED(pFileSave->GetResult(&pResultItem)))
            {
                PWSTR pszFilePath = nullptr;
                if (SUCCEEDED(pResultItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath)))
                {
                    chosenZipPath = pszFilePath;
                    CoTaskMemFree(pszFilePath);
                }
                pResultItem->Release();
            }
        }
        pFileSave->Release();
    }

    if (chosenZipPath.empty())
        return;

    std::wstring pathList = L"";
    for (size_t i = 0; i < selectedPaths.size(); ++i)
    {
        pathList += L"'" + selectedPaths[i] + L"'";
        if (i + 1 < selectedPaths.size())
            pathList += L",";
    }

    std::wstring psCmd = L"/c powershell -NoProfile -Command \"Compress-Archive -Path " + pathList + L" -DestinationPath '" + chosenZipPath + L"' -Force\"";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = psCmd.c_str();
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei) && sei.hProcess)
    {
        WaitForSingleObject(sei.hProcess, 6000);
        CloseHandle(sei.hProcess);
        AddFileToTray(chosenZipPath, true);
    }

    CalculateTargetSize(g_isExpanded);
    SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void DeleteSelectedItems(HWND hwnd)
{
    auto it = g_items.begin();
    while (it != g_items.end())
    {
        if (it->isSelected)
        {
            if (it->icon)
                DestroyIcon(it->icon);
            it = g_items.erase(it);
        }
        else
        {
            ++it;
        }
    }
    g_hoveredIndex = -1;
    g_lastClickedIndex = -1;
    CalculateTargetSize(g_isExpanded);
    SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void TogglePinSelectedItems(HWND hwnd)
{
    bool hasUnpinned = false;
    for (const auto &item : g_items)
    {
        if (item.isSelected && !item.isPinned)
        {
            hasUnpinned = true;
            break;
        }
    }
    for (auto &item : g_items)
    {
        if (item.isSelected)
        {
            item.isPinned = hasUnpinned;
        }
    }
    CalculateTargetSize(g_isExpanded);
    SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void HandleFileDrop(HDROP hDrop, HWND hwnd)
{
    if (g_isSelfDragging)
    {
        DragFinish(hDrop);
        return;
    }

    UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < fileCount; ++i)
    {
        wchar_t filePath[MAX_PATH];
        if (DragQueryFileW(hDrop, i, filePath, MAX_PATH))
        {
            AddFileToTray(filePath, false);
        }
    }
    DragFinish(hDrop);
    CalculateTargetSize(g_isExpanded);
    SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
}

void StartDraggingSelectedOut(HWND hwnd, int clickedIdx)
{
    std::vector<std::wstring> pathsToDrag;
    std::vector<int> indicesToDrag;

    for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
    {
        if (g_items[i].isSelected)
        {
            pathsToDrag.push_back(g_items[i].path);
            indicesToDrag.push_back(i);
        }
    }

    if (pathsToDrag.empty() && clickedIdx >= 0 && clickedIdx < static_cast<int>(g_items.size()))
    {
        pathsToDrag.push_back(g_items[clickedIdx].path);
        indicesToDrag.push_back(clickedIdx);
    }

    if (pathsToDrag.empty())
        return;

    g_isSelfDragging = true;
    DragAcceptFiles(hwnd, FALSE);

    DWORD dropEffect = g_settings.moveFiles ? DROPEFFECT_MOVE : DROPEFFECT_COPY;

    auto *dataObj = new MultiFileDataObject(pathsToDrag, dropEffect);
    auto *dropSrc = new SimpleDropSource();

    DWORD dwEffectResult = 0;
    HRESULT hr = DoDragDrop(dataObj, dropSrc, dropEffect, &dwEffectResult);

    dataObj->Release();
    dropSrc->Release();

    // Auto-remove unpinned files if drop succeeded and enabled in settings
    if (hr == DRAGDROP_S_DROP && (dwEffectResult & (DROPEFFECT_COPY | DROPEFFECT_MOVE)))
    {
        if (g_settings.autoRemoveUnpinned || g_settings.moveFiles)
        {
            std::vector<std::wstring> toRemove;
            for (int idx : indicesToDrag)
            {
                if (idx >= 0 && idx < static_cast<int>(g_items.size()))
                {
                    if (!g_items[idx].isPinned)
                    {
                        toRemove.push_back(g_items[idx].path);
                    }
                }
            }

            for (const auto &removePath : toRemove)
            {
                auto it = std::find_if(g_items.begin(), g_items.end(), [&](const PinnedItem &item)
                                       { return item.path == removePath; });
                if (it != g_items.end())
                {
                    if (it->icon)
                        DestroyIcon(it->icon);
                    g_items.erase(it);
                }
            }
            g_hoveredIndex = -1;
            g_lastClickedIndex = -1;
            CalculateTargetSize(g_isExpanded);
            SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
        }
    }

    DragAcceptFiles(hwnd, TRUE);
    g_isSelfDragging = false;
    InvalidateRect(hwnd, nullptr, TRUE);
}

// -------------------------------------------------------------
// Window Procedure
// -------------------------------------------------------------
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        InitSettingsPath();
        LoadSettings();
        ApplyWindowBackdrop(hwnd, BackdropType::Acrylic);
        DragAcceptFiles(hwnd, TRUE);
        CalculateTargetSize(false);

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int posX = (screenWidth - static_cast<int>(COLLAPSED_W)) / 2;
        SetWindowPos(hwnd, HWND_TOPMOST, posX, 0, static_cast<int>(COLLAPSED_W), static_cast<int>(COLLAPSED_H), SWP_NOACTIVATE);
        return 0;
    }

    case WM_KEYDOWN:
    {
        if (wParam == VK_DELETE)
        {
            DeleteSelectedItems(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == ANIM_TIMER_ID)
        {
            bool needsRedraw = false;

            float speedFactor = g_isExpanded ? 0.20f : 0.12f;
            float diffW = g_targetW - g_curW;
            float diffH = g_targetH - g_curH;

            if (std::abs(diffW) > 0.5f || std::abs(diffH) > 0.5f)
            {
                g_curW += diffW * speedFactor;
                g_curH += diffH * speedFactor;
                needsRedraw = true;
            }
            else
            {
                g_curW = g_targetW;
                g_curH = g_targetH;
            }

            float diffScroll = g_targetScroll - g_scrollOffset;
            if (std::abs(diffScroll) > 0.2f)
            {
                g_scrollOffset += diffScroll * 0.25f;
                needsRedraw = true;
            }
            else
            {
                g_scrollOffset = g_targetScroll;
            }

            for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
            {
                float targetScale = (i == g_hoveredIndex && g_isExpanded && g_settings.enableMagnify) ? 1.25f : 1.0f;
                float diffScale = targetScale - g_items[i].currentScale;
                if (std::abs(diffScale) > 0.01f)
                {
                    g_items[i].currentScale += diffScale * 0.28f;
                    needsRedraw = true;
                }
                else
                {
                    g_items[i].currentScale = targetScale;
                }
            }

            if (!needsRedraw && g_curW == g_targetW && g_curH == g_targetH)
            {
                KillTimer(hwnd, ANIM_TIMER_ID);
            }

            int w = static_cast<int>(std::round(g_curW));
            int h = static_cast<int>(std::round(g_curH));
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int posX = (screenWidth - w) / 2;

            SetWindowPos(hwnd, HWND_TOPMOST, posX, 0, w, h, SWP_NOACTIVATE | SWP_NOREDRAW);

            HRGN hRgn = CreateRoundRectRgn(0, -14, w + 1, h + 1, CORNER_RADIUS, CORNER_RADIUS);
            SetWindowRgn(hwnd, hRgn, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        if (g_isExpanded && g_maxScroll > 0.0f)
        {
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            g_targetScroll -= (zDelta / WHEEL_DELTA) * 85.0f;
            g_targetScroll = (std::max)(0.0f, (std::min)(g_targetScroll, g_maxScroll));
            SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        if (!g_mouseTrackingActive)
        {
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            g_mouseTrackingActive = true;

            if (!g_isExpanded)
            {
                g_isExpanded = true;
                PruneMissingItems(hwnd);
                CalculateTargetSize(true);
                SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
            }
        }

        POINT pt = {x, y};
        bool newSettingsHover = PtInRect(&g_settingsBtnRect, pt);
        if (newSettingsHover != g_settingsHovered)
        {
            g_settingsHovered = newSettingsHover;
            InvalidateRect(hwnd, &g_settingsBtnRect, FALSE);
        }

        if (g_isTrackingDrag && (wParam & MK_LBUTTON))
        {
            if (abs(x - g_dragStartPoint.x) > 6 || abs(y - g_dragStartPoint.y) > 6)
            {
                int dragIdx = g_dragCandidateIndex;
                g_isTrackingDrag = false;
                g_dragCandidateIndex = -1;
                StartDraggingSelectedOut(hwnd, dragIdx);
                return 0;
            }
        }

        int newHover = -1;
        for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
        {
            if (PtInRect(&g_items[i].rect, pt))
            {
                newHover = i;
                break;
            }
        }

        if (newHover != g_hoveredIndex)
        {
            g_hoveredIndex = newHover;
            SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
    {
        g_mouseTrackingActive = false;
        g_hoveredIndex = -1;
        g_settingsHovered = false;
        g_isTrackingDrag = false;
        g_dragCandidateIndex = -1;

        if (!g_isMenuOpen && g_isExpanded)
        {
            g_isExpanded = false;
            g_scrollOffset = 0.0f;
            g_targetScroll = 0.0f;
            CalculateTargetSize(false);
            SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        SetFocus(hwnd);
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        POINT pt = {x, y};

        if (PtInRect(&g_settingsBtnRect, pt))
        {
            OpenSettingsDialog(hwnd);
            CalculateTargetSize(g_isExpanded);
            SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
            return 0;
        }

        bool clickedAnItem = false;
        for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
        {
            if (PtInRect(&g_items[i].rect, pt))
            {
                clickedAnItem = true;
                g_isTrackingDrag = true;
                g_dragCandidateIndex = i;
                g_dragStartPoint = pt;

                bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

                if (isShift && g_lastClickedIndex != -1)
                {
                    int startIdx = (std::min)(g_lastClickedIndex, i);
                    int endIdx = (std::max)(g_lastClickedIndex, i);
                    if (!isCtrl)
                    {
                        for (auto &item : g_items)
                            item.isSelected = false;
                    }
                    for (int k = startIdx; k <= endIdx; ++k)
                    {
                        g_items[k].isSelected = true;
                    }
                }
                else if (isCtrl)
                {
                    g_items[i].isSelected = !g_items[i].isSelected;
                    g_lastClickedIndex = i;
                }
                else
                {
                    if (!g_items[i].isSelected)
                    {
                        for (auto &item : g_items)
                            item.isSelected = false;
                        g_items[i].isSelected = true;
                    }
                    g_lastClickedIndex = i;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
        }

        if (!clickedAnItem)
        {
            for (auto &item : g_items)
                item.isSelected = false;
            g_lastClickedIndex = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
    {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        POINT pt = {x, y};

        for (const auto &item : g_items)
        {
            if (PtInRect(&item.rect, pt))
            {
                PruneMissingItems(hwnd);
                ShellExecuteW(nullptr, L"open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        g_isTrackingDrag = false;
        g_dragCandidateIndex = -1;
        return 0;
    }

    case WM_RBUTTONUP:
    {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        POINT pt = {x, y};

        int clickedIdx = -1;
        for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
        {
            if (PtInRect(&g_items[i].rect, pt))
            {
                clickedIdx = i;
                break;
            }
        }

        if (clickedIdx != -1)
        {
            if (!g_items[clickedIdx].isSelected)
            {
                if (!(GetKeyState(VK_CONTROL) & 0x8000))
                {
                    for (auto &item : g_items)
                        item.isSelected = false;
                }
                g_items[clickedIdx].isSelected = true;
                g_lastClickedIndex = clickedIdx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            int selectedCount = 0;
            for (const auto &it : g_items)
                if (it.isSelected)
                    selectedCount++;

            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1001, L"Open");
            AppendMenuW(hMenu, MF_STRING, 1002, L"Show in File Explorer");

            std::wstring pinLabel = (selectedCount > 1) ? L"Toggle Pin for Selected" : (g_items[clickedIdx].isPinned ? L"Unpin from Tray" : L"Pin to Tray");
            AppendMenuW(hMenu, MF_STRING, 1003, pinLabel.c_str());

            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 1004, (selectedCount > 1) ? L"Compress Selected to ZIP..." : L"Compress to ZIP...");
            AppendMenuW(hMenu, MF_STRING, 1005, (selectedCount > 1) ? L"Delete Selected (Del)" : L"Remove from Tray (Del)");

            POINT screenPt = pt;
            ClientToScreen(hwnd, &screenPt);

            g_isMenuOpen = true;
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            g_isMenuOpen = false;

            if (cmd == 1001)
            {
                PruneMissingItems(hwnd);
                for (const auto &item : g_items)
                {
                    if (item.isSelected)
                    {
                        ShellExecuteW(nullptr, L"open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
            }
            else if (cmd == 1002)
            {
                std::wstring params = L"/select,\"" + g_items[clickedIdx].path + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
            }
            else if (cmd == 1003)
            {
                TogglePinSelectedItems(hwnd);
            }
            else if (cmd == 1004)
            {
                CompressSelectedToZip(hwnd);
            }
            else if (cmd == 1005)
            {
                DeleteSelectedItems(hwnd);
            }
            return 0;
        }
        return 0;
    }

    case WM_DROPFILES:
        HandleFileDrop(reinterpret_cast<HDROP>(wParam), hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_HOTKEY: {
    if (wParam == 9001) { // HOTKEY_TOGGLE_ID
        if (IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_HIDE);
        } else {
            ShowWindow(hwnd, SW_SHOW);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    return 0;
}

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        int width = clientRc.right;
        int height = clientRc.bottom;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
        HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

        // Pure black RGB(0,0,0) punches the alpha channel through for DWM Acrylic composition:
        HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &clientRc, bgBrush);
        DeleteObject(bgBrush);

        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 70));
        HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
        SelectObject(memDC, borderPen);
        SelectObject(memDC, nullBrush);
        RoundRect(memDC, 0, -14, width, height, CORNER_RADIUS, CORNER_RADIUS);
        DeleteObject(borderPen);

        if (g_curH < 30.0f)
        {
            HBRUSH pillBrush = CreateSolidBrush(RGB(75, 75, 82));
            SelectObject(memDC, pillBrush);
            RoundRect(memDC, (width - 36) / 2, 4, (width + 36) / 2, 8, 4, 4);
            DeleteObject(pillBrush);
        }
        else
        {
            // Render Top-Right Settings Gear Pill (⚙)
            g_settingsBtnRect = {width - 38, 12, width - 14, 36};
            if (g_settingsHovered)
            {
                HBRUSH gearHover = CreateSolidBrush(RGB(48, 48, 58));
                HPEN gearPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 95));
                SelectObject(memDC, gearHover);
                SelectObject(memDC, gearPen);
                RoundRect(memDC, g_settingsBtnRect.left, g_settingsBtnRect.top, g_settingsBtnRect.right, g_settingsBtnRect.bottom, 8, 8);
                DeleteObject(gearHover);
                DeleteObject(gearPen);
            }
            HFONT gearFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
            HFONT oldGFont = (HFONT)SelectObject(memDC, gearFont);
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, g_settingsHovered ? RGB(255, 255, 255) : RGB(160, 160, 175));
            DrawTextW(memDC, L"\x2699", -1, &g_settingsBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            SelectObject(memDC, oldGFont);
            DeleteObject(gearFont);

            if (g_items.empty())
            {
                HFONT hFont = CreateFontW(14, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
                HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, RGB(160, 160, 170));

                RECT textRc = clientRc;
                DrawTextW(memDC, L"Drop files or applications here", -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                SelectObject(memDC, oldFont);
                DeleteObject(hFont);
            }
            else
            {
                int baseItemW = g_settings.largeIcons ? 68 : 60;
                int baseItemH = g_settings.largeIcons ? 76 : 68;
                int baseIconSize = g_settings.largeIcons ? 40 : 32;
                int itemSpacing = 8;
                int startY = 14;

                std::vector<int> pinnedIndices;
                std::vector<int> unpinnedIndices;
                for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
                {
                    if (g_items[i].isPinned)
                        pinnedIndices.push_back(i);
                    else
                        unpinnedIndices.push_back(i);
                }

                int currentX = 18;

                HFONT hFont = CreateFontW(g_settings.largeIcons ? -12 : -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
                SetBkMode(memDC, TRANSPARENT);

                // --- SECTION 1: PINNED ITEMS ---
                if (!pinnedIndices.empty())
                {
                    for (int idx : pinnedIndices)
                    {
                        float scale = g_items[idx].currentScale;
                        int itemW = static_cast<int>(baseItemW * scale);
                        int itemH = static_cast<int>(baseItemH * scale);
                        int shiftY = (itemH - baseItemH) / 2;

                        RECT pillRect = {currentX, startY - shiftY, currentX + itemW, startY + baseItemH + shiftY};
                        g_items[idx].rect = pillRect;

                        if (g_items[idx].isSelected)
                        {
                            HBRUSH selBrush = CreateSolidBrush(RGB(10, 85, 175));
                            HPEN borderSel = CreatePen(PS_SOLID, 1, RGB(45, 140, 245));
                            SelectObject(memDC, selBrush);
                            SelectObject(memDC, borderSel);
                            RoundRect(memDC, pillRect.left, pillRect.top, pillRect.right, pillRect.bottom, 12, 12);
                            DeleteObject(selBrush);
                            DeleteObject(borderSel);
                        }
                        else if (idx == g_hoveredIndex)
                        {
                            HBRUSH hoverBrush = CreateSolidBrush(RGB(40, 40, 48));
                            HPEN borderPill = CreatePen(PS_SOLID, 1, RGB(70, 70, 85));
                            SelectObject(memDC, hoverBrush);
                            SelectObject(memDC, borderPill);
                            RoundRect(memDC, pillRect.left, pillRect.top, pillRect.right, pillRect.bottom, 12, 12);
                            DeleteObject(hoverBrush);
                            DeleteObject(borderPill);
                        }

                        if (g_items[idx].icon)
                        {
                            int iconSize = static_cast<int>(baseIconSize * scale);
                            int iconX = currentX + (itemW - iconSize) / 2;
                            int iconY = startY + 6 - shiftY;
                            DrawIconEx(memDC, iconX, iconY, g_items[idx].icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);

                            HBRUSH pinBadge = CreateSolidBrush(RGB(245, 158, 11));
                            HPEN pinRing = CreatePen(PS_SOLID, 1, RGB(180, 100, 10));
                            SelectObject(memDC, pinBadge);
                            SelectObject(memDC, pinRing);
                            RoundRect(memDC, iconX + iconSize - 8, iconY, iconX + iconSize + 2, iconY + 10, 3, 3);
                            DeleteObject(pinBadge);
                            DeleteObject(pinRing);

                            RECT textRc = {currentX + 2, startY + (g_settings.largeIcons ? 50 : 44) + shiftY, currentX + itemW - 2, startY + baseItemH - 6 + shiftY};
                            SetTextColor(memDC, g_items[idx].isSelected ? RGB(255, 255, 255) : RGB(215, 215, 225));
                            DrawTextW(memDC, g_items[idx].name.c_str(), -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_END_ELLIPSIS);
                        }
                        currentX += itemW + itemSpacing;
                    }

                    if (!unpinnedIndices.empty())
                    {
                        HPEN divPen = CreatePen(PS_SOLID, 1, RGB(55, 55, 65));
                        HPEN oldPen = (HPEN)SelectObject(memDC, divPen);
                        MoveToEx(memDC, currentX + 6, startY + 8, nullptr);
                        LineTo(memDC, currentX + 6, startY + baseItemH - 8);
                        SelectObject(memDC, oldPen);
                        DeleteObject(divPen);
                        currentX += 20;
                    }
                }

                // --- SECTION 2: TEMPORARY ITEMS ---
                if (!unpinnedIndices.empty())
                {
                    int unpinnedStartX = currentX;
                    int availableWidth = width - unpinnedStartX - 48; // Leave room for settings gear
                    int totalUnpinnedW = static_cast<int>(unpinnedIndices.size()) * (baseItemW + itemSpacing);
                    g_maxScroll = static_cast<float>((std::max)(0, totalUnpinnedW - availableWidth));

                    HRGN clipRgn = CreateRectRgn(unpinnedStartX - 4, startY - 8, width - 44, startY + baseItemH + 14);
                    SelectClipRgn(memDC, clipRgn);

                    int scrollX = unpinnedStartX - static_cast<int>(std::round(g_scrollOffset));
                    for (int idx : unpinnedIndices)
                    {
                        float scale = g_items[idx].currentScale;
                        int itemW = static_cast<int>(baseItemW * scale);
                        int itemH = static_cast<int>(baseItemH * scale);
                        int shiftY = (itemH - baseItemH) / 2;

                        RECT pillRect = {scrollX, startY - shiftY, scrollX + itemW, startY + baseItemH + shiftY};
                        g_items[idx].rect = pillRect;

                        if (g_items[idx].isSelected)
                        {
                            HBRUSH selBrush = CreateSolidBrush(RGB(10, 85, 175));
                            HPEN borderSel = CreatePen(PS_SOLID, 1, RGB(45, 140, 245));
                            SelectObject(memDC, selBrush);
                            SelectObject(memDC, borderSel);
                            RoundRect(memDC, pillRect.left, pillRect.top, pillRect.right, pillRect.bottom, 12, 12);
                            DeleteObject(selBrush);
                            DeleteObject(borderSel);
                        }
                        else if (idx == g_hoveredIndex)
                        {
                            HBRUSH hoverBrush = CreateSolidBrush(RGB(40, 40, 48));
                            HPEN borderPill = CreatePen(PS_SOLID, 1, RGB(70, 70, 85));
                            SelectObject(memDC, hoverBrush);
                            SelectObject(memDC, borderPill);
                            RoundRect(memDC, pillRect.left, pillRect.top, pillRect.right, pillRect.bottom, 12, 12);
                            DeleteObject(hoverBrush);
                            DeleteObject(borderPill);
                        }

                        if (g_items[idx].icon)
                        {
                            int iconSize = static_cast<int>(baseIconSize * scale);
                            int iconX = scrollX + (itemW - iconSize) / 2;
                            int iconY = startY + 6 - shiftY;
                            DrawIconEx(memDC, iconX, iconY, g_items[idx].icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);

                            RECT textRc = {scrollX + 2, startY + (g_settings.largeIcons ? 50 : 44) + shiftY, scrollX + itemW - 2, startY + baseItemH - 6 + shiftY};
                            SetTextColor(memDC, g_items[idx].isSelected ? RGB(255, 255, 255) : RGB(185, 185, 195));
                            DrawTextW(memDC, g_items[idx].name.c_str(), -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_END_ELLIPSIS);
                        }
                        scrollX += itemW + itemSpacing;
                    }

                    SelectClipRgn(memDC, nullptr);
                    DeleteObject(clipRgn);

                    if (g_maxScroll > 0.0f)
                    {
                        int trackW = availableWidth;
                        int thumbW = (std::max)(22, trackW * availableWidth / totalUnpinnedW);
                        int thumbX = unpinnedStartX + static_cast<int>(std::round((trackW - thumbW) * (g_scrollOffset / g_maxScroll)));

                        HBRUSH thumbBrush = CreateSolidBrush(RGB(90, 90, 105));
                        HPEN noPen = CreatePen(PS_NULL, 0, 0);
                        SelectObject(memDC, thumbBrush);
                        SelectObject(memDC, noPen);
                        RoundRect(memDC, thumbX, startY + baseItemH + 12, thumbX + thumbW, startY + baseItemH + 15, 2, 2);
                        DeleteObject(thumbBrush);
                        DeleteObject(noPen);
                    }
                }

                // --- HOVER TOOLTIP CARD ---
                if (g_hoveredIndex >= 0 && g_hoveredIndex < static_cast<int>(g_items.size()))
                {
                    const auto &item = g_items[g_hoveredIndex];

                    HFONT tipFont = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                    SelectObject(memDC, tipFont);

                    RECT calcRc = {0, 0, 0, 0};
                    DrawTextW(memDC, item.name.c_str(), -1, &calcRc, DT_CALCRECT | DT_SINGLELINE);
                    int tipWidth = (calcRc.right - calcRc.left) + 24;
                    int tipHeight = 22;

                    int tipX = item.rect.left + ((item.rect.right - item.rect.left) - tipWidth) / 2;
                    tipX = (std::max)(12, (std::min)(tipX, width - tipWidth - 12));
                    int tipY = height - 26;

                    HBRUSH tipBg = CreateSolidBrush(RGB(32, 32, 38));
                    HPEN tipBorder = CreatePen(PS_SOLID, 1, RGB(75, 75, 90));
                    SelectObject(memDC, tipBg);
                    SelectObject(memDC, tipBorder);
                    RoundRect(memDC, tipX, tipY, tipX + tipWidth, tipY + tipHeight, 10, 10);
                    DeleteObject(tipBg);
                    DeleteObject(tipBorder);

                    RECT labelRc = {tipX, tipY, tipX + tipWidth, tipY + tipHeight};
                    SetTextColor(memDC, RGB(255, 255, 255));
                    DrawTextW(memDC, item.name.c_str(), -1, &labelRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                    DeleteObject(tipFont);
                }

                SelectObject(memDC, oldFont);
                DeleteObject(hFont);
            }
        }

        BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ANIM_TIMER_ID);
        for (auto &item : g_items)
        {
            if (item.icon)
                DestroyIcon(item.icon);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    OleInitialize(nullptr);

    const wchar_t CLASS_NAME[] = L"WinTrayAppWindow";

    WNDCLASS wc = {};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    RegisterClass(&wc);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int posX = (screenWidth - static_cast<int>(COLLAPSED_W)) / 2;

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME,
        L"WinTray",
        WS_POPUP | WS_VISIBLE,
        posX, 0, static_cast<int>(COLLAPSED_W), static_cast<int>(COLLAPSED_H),
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
        return 0;

    // Register Win + Alt + T global hotkey to show/hide the tray
    constexpr int HOTKEY_TOGGLE_ID = 9001;
    RegisterHotKey(hwnd, HOTKEY_TOGGLE_ID, MOD_WIN | MOD_ALT, 'T');

    HRGN hRgn = CreateRoundRectRgn(0, -14, static_cast<int>(COLLAPSED_W) + 1, static_cast<int>(COLLAPSED_H) + 1, CORNER_RADIUS, CORNER_RADIUS);
    SetWindowRgn(hwnd, hRgn, TRUE);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Clean up hotkey registration before exit
    UnregisterHotKey(hwnd, HOTKEY_TOGGLE_ID);

    OleUninitialize();
    return 0;
}