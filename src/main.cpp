#include "state.h"
#include "settings.h"
#include "updater.h"
#include "media.h"
#include "blur.h"

#include <shellapi.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <wincodec.h> // Robust image decoding (WebP, JPG, PNG)
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <algorithm>

#pragma comment(lib, "Windowscodecs.lib")

HWND g_mainHwnd = nullptr;
AppSettings g_settings;
ThemePalette g_theme;
std::vector<PinnedItem> g_items;
bool g_isExpanded = false;
bool g_isSelfDragging = false;
float g_SpinnerAngle = 0.0f;

static int g_nextItemId = 1;
static int g_hoveredIndex = -1;
static int g_lastClickedIndex = -1;
static POINT g_dragStartPoint = {};
static bool g_isTrackingDrag = false;
static int g_dragCandidateIndex = -1;
static bool g_isMenuOpen = false;
static bool g_mouseTrackingActive = false;
static bool g_isDraggingWindow = false;

static RECT g_settingsBtnRect = {};
static bool g_settingsHovered = false;

static float g_curW = COLLAPSED_W;
static float g_curH = COLLAPSED_H;
static float g_targetW = COLLAPSED_W;
static float g_targetH = COLLAPSED_H;

static float g_scrollOffset = 0.0f;
static float g_targetScroll = 0.0f;
static float g_maxScroll = 0.0f;


bool IsResolvedDarkMode()
{
    if (g_settings.themeMode == 1) return false; // Light Mode
    if (g_settings.themeMode == 2) return true;  // Dark Mode
    
    DWORD value = 0, size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return (value == 0);
    }
    return true; 
}

void UpdateThemeColors()
{
    bool isDark = IsResolvedDarkMode();
    
    // Borders: Subtle definition
    g_theme.border       = isDark ? RGB(50, 50, 60)   : RGB(170, 175, 185);
    
    // TEXT: Rich Charcoal / Deep Navy for razor-sharp Light Mode readability
g_theme.textPrimary  = isDark ? RGB(255, 255, 255): RGB(255, 255, 255); 
    g_theme.textSec      = isDark ? RGB(160, 160, 175): RGB(65, 75, 90);
    
    // Hover Pills: Soft, translucent slate (ensures the dark text pops)
    g_theme.pillHover    = isDark ? RGB(35, 35, 42)   : RGB(35, 35, 42);
    g_theme.pillBorder   = isDark ? RGB(70, 70, 85)   : RGB(35, 35, 42);
    
    // Selection: Crisp Windows Blue (White text drawn on top of this will be perfectly readable)
    g_theme.selBg        = isDark ? RGB(15, 45, 85)   : RGB(0, 0, 0);
    g_theme.selBorder    = isDark ? RGB(45, 140, 245) : RGB(0, 85, 180);
    
    // Tooltips & Dividers
    g_theme.tipBg        = isDark ? RGB(32, 32, 38)   : RGB(0, 0, 0) ;
    g_theme.div          = isDark ? RGB(55, 55, 65)   : RGB(180, 185, 195);
    
    // Scrollbars
    g_theme.scrollTrack  = isDark ? RGB(20, 20, 25)   : RGB(205, 210, 220);
    g_theme.scrollThumb  = isDark ? RGB(90, 90, 105)  : RGB(140, 145, 155);
}
// Bulletproof path normalizer for VS Code & Web browsers
static std::wstring NormalizeWinPath(const std::wstring &rawPath)
{
    std::wstring p = rawPath;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (p.length() >= 3 && p[0] == L'\\' && iswalpha(p[1]) && (p[2] == L':' || p[2] == L'|'))
        p.erase(0, 1);
    if (p.length() >= 2 && iswalpha(p[0]) && p[1] == L'|')
        p[1] = L':';
    if (p.length() >= 2 && iswalpha(p[0]) && p[1] == L':')
        p[0] = towupper(p[0]);
    return p;
}

// Generate rich thumbnails instantly (Supports WebP, JPG, PNG natively via WIC)
HICON GetIconOrThumbnail(const std::wstring &path, bool largeIcon)
{
    int size = largeIcon ? 64 : 48;
    HICON hIcon = nullptr;
    IShellItemImageFactory *pFactory = nullptr;

    // 1. Try Shell Thumbnail (SIIGBF_EXTRACTIFNEEDED forces extraction for new files)
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&pFactory))))
    {
        SIZE sz = {size, size};
        HBITMAP hBmp = nullptr;
        // 0x00000020 is SIIGBF_EXTRACTIFNEEDED
        if (SUCCEEDED(pFactory->GetImage(sz, static_cast<SIIGBF>(0x00000020), &hBmp)) && hBmp)
        {
            HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, nullptr);
            ICONINFO ii = {TRUE, 0, 0, hbmMask, hBmp};
            hIcon = CreateIconIndirect(&ii);
            DeleteObject(hbmMask);
            DeleteObject(hBmp);
        }
        pFactory->Release();
    }

    // 2. Fallback to WIC (Instant WebP/JPG/PNG decoding)
    if (!hIcon)
    {
        IWICImagingFactory *pWICFactory = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory))))
        {
            IWICBitmapDecoder *pDecoder = nullptr;
            if (SUCCEEDED(pWICFactory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder)))
            {
                IWICBitmapFrameDecode *pFrame = nullptr;
                if (SUCCEEDED(pDecoder->GetFrame(0, &pFrame)))
                {
                    IWICFormatConverter *pConverter = nullptr;
                    if (SUCCEEDED(pWICFactory->CreateFormatConverter(&pConverter)))
                    {
                        pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom);

                        IWICBitmapScaler *pScaler = nullptr;
                        if (SUCCEEDED(pWICFactory->CreateBitmapScaler(&pScaler)))
                        {
                            UINT w = 0, h = 0;
                            pFrame->GetSize(&w, &h);
                            float aspect = static_cast<float>(w) / static_cast<float>(h);
                            UINT targetW = size, targetH = size;
                            if (aspect > 1.0f)
                                targetH = static_cast<UINT>(size / aspect);
                            else
                                targetW = static_cast<UINT>(size * aspect);

                            pScaler->Initialize(pConverter, targetW, targetH, WICBitmapInterpolationModeFant);

                            BITMAPINFO bmi = {};
                            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                            bmi.bmiHeader.biWidth = targetW;
                            bmi.bmiHeader.biHeight = -static_cast<LONG>(targetH);
                            bmi.bmiHeader.biPlanes = 1;
                            bmi.bmiHeader.biBitCount = 32;
                            bmi.bmiHeader.biCompression = BI_RGB;

                            void *pPixels = nullptr;
                            HDC hdc = GetDC(nullptr);
                            HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pPixels, nullptr, 0);
                            ReleaseDC(nullptr, hdc);

                            if (hBmp && pPixels)
                            {
                                pScaler->CopyPixels(nullptr, targetW * 4, targetW * targetH * 4, static_cast<BYTE *>(pPixels));
                                HBITMAP hbmMask = CreateBitmap(targetW, targetH, 1, 1, nullptr);
                                ICONINFO ii = {TRUE, 0, 0, hbmMask, hBmp};
                                hIcon = CreateIconIndirect(&ii);
                                DeleteObject(hbmMask);
                                DeleteObject(hBmp);
                            }
                            pScaler->Release();
                        }
                        pConverter->Release();
                    }
                    pFrame->Release();
                }
                pDecoder->Release();
            }
            pWICFactory->Release();
        }
    }

    // 3. Ultimate Fallback: System File Icon
    if (!hIcon)
    {
        SHFILEINFOW sfi = {};
        SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | (largeIcon ? SHGFI_LARGEICON : 0));
        hIcon = sfi.hIcon;
    }
    return hIcon;
}

void ApplyGlobalHotkey(HWND hwnd)
{
    UnregisterHotKey(hwnd, HOTKEY_TOGGLE_ID);
    BOOL registered = RegisterHotKey(hwnd, HOTKEY_TOGGLE_ID, g_settings.hotkeyMods | MOD_NOREPEAT, g_settings.hotkeyVk);
    if (!registered)
        RegisterHotKey(hwnd, HOTKEY_TOGGLE_ID, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'T');
}

void TriggerExpand(HWND hwnd)
{
    if (!g_isExpanded)
    {
        g_isExpanded = true;
        PruneMissingItems(hwnd);
        CalculateTargetSize(true);
        SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
    }
}

void CalculateTargetSize(bool expanded)
{
    bool isVertical = (g_settings.dockEdge == EDGE_LEFT || g_settings.dockEdge == EDGE_RIGHT);
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

        int itemStep = isVertical ? (g_settings.largeIcons ? 84 : 76) : (g_settings.largeIcons ? 74 : 64);
        int totalItems = pinnedCount + unpinnedCount;
        int basePadding = 90 + ((pinnedCount > 0 && unpinnedCount > 0) ? 24 : 0);
        int contentLen = std::max(460, totalItems * itemStep + basePadding);

        int maxScreenLen = isVertical ? (GetSystemMetrics(SM_CYSCREEN) - 120) : 980;
        float len = static_cast<float>(std::min(contentLen, std::max(460, maxScreenLen)));
        float thick = g_settings.largeIcons ? 136.0f : 124.0f;

        g_targetW = isVertical ? thick : len;
        g_targetH = isVertical ? len : thick;
    }
    else
    {
        g_targetW = isVertical ? COLLAPSED_H : COLLAPSED_W;
        g_targetH = isVertical ? COLLAPSED_W : COLLAPSED_H;
    }
}

void UpdateTrayWindowGeometry(HWND hwnd)
{
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    int w = static_cast<int>(std::lround(g_curW));
    int h = static_cast<int>(std::lround(g_curH));
    int posX = 0, posY = 0;

    switch (g_settings.dockEdge)
    {
    case EDGE_TOP:
        posX = std::clamp(static_cast<int>(screenW * g_settings.dockOffsetRatio - w / 2), 0, screenW - w);
        posY = 0;
        break;
    case EDGE_LEFT:
        posX = 0;
        posY = std::clamp(static_cast<int>(screenH * g_settings.dockOffsetRatio - h / 2), 0, screenH - h);
        break;
    case EDGE_RIGHT:
        posX = screenW - w;
        posY = std::clamp(static_cast<int>(screenH * g_settings.dockOffsetRatio - h / 2), 0, screenH - h);
        break;
    }

    SetWindowPos(hwnd, HWND_TOPMOST, posX, posY, w, h, SWP_NOACTIVATE | SWP_NOREDRAW);

    // Restore perfect manual clipping to eliminate the double-border misalignment!
    HRGN hRgn = nullptr;
    if (g_settings.dockEdge == EDGE_TOP)
        hRgn = CreateRoundRectRgn(0, -14, w + 1, h + 1, CORNER_RADIUS, CORNER_RADIUS);
    else if (g_settings.dockEdge == EDGE_LEFT)
        hRgn = CreateRoundRectRgn(-14, 0, w + 1, h + 1, CORNER_RADIUS, CORNER_RADIUS);
    else
        hRgn = CreateRoundRectRgn(0, 0, w + 15, h + 1, CORNER_RADIUS, CORNER_RADIUS);

    SetWindowRgn(hwnd, hRgn, TRUE);
}

void PruneMissingItems(HWND hwnd)
{
    bool changed = false;
    auto it = g_items.begin();
    while (it != g_items.end())
    {
        if (!it->isLoading && GetFileAttributesW(it->path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            if (it->icon)
                DestroyIcon(it->icon);
            it = g_items.erase(it);
            changed = true;
        }
        else
            ++it;
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

int AddPlaceholderItem(const std::wstring &tempName)
{
    PinnedItem item;
    item.id = g_nextItemId++;
    item.name = tempName;
    item.isLoading = true;
    item.currentScale = 1.0f;
    g_items.push_back(item);

    CalculateTargetSize(g_isExpanded);
    SetTimer(g_mainHwnd, ANIM_TIMER_ID, 12, nullptr);
    InvalidateRect(g_mainHwnd, nullptr, TRUE);
    return item.id;
}

void AddFileToTray(const std::wstring &rawFilePath, bool isPinned, const std::wstring &customName)
{
    std::wstring filePath = NormalizeWinPath(rawFilePath);
    if (filePath.empty() || GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    for (const auto &item : g_items)
    {
        if (!item.isLoading && _wcsicmp(item.path.c_str(), filePath.c_str()) == 0)
            return;
    }

    PinnedItem item;
    item.id = g_nextItemId++;
    item.path = filePath;

    if (!customName.empty())
    {
        item.name = customName;
    }
    else
    {
        SHFILEINFOW sfi = {};
        SHGetFileInfoW(filePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME);
        item.name = (sfi.szDisplayName[0] != L'\0') ? sfi.szDisplayName : filePath.substr(filePath.find_last_of(L"\\/") + 1);
    }

    item.icon = GetIconOrThumbnail(filePath, g_settings.largeIcons);
    item.isSelected = false;
    item.isPinned = isPinned;
    item.isLoading = false;
    item.currentScale = 1.0f;
    g_items.push_back(item);
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
            DeleteTempFileIfOwned(it->path);
            it = g_items.erase(it);
        }
        else
            ++it;
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
            item.isPinned = hasUnpinned;
    }
    CalculateTargetSize(g_isExpanded);
    SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void DrawLoadingSpinner(HDC hdc, int centerX, int centerY, int radius, float angleDeg)
{
    HPEN trackPen = CreatePen(PS_SOLID, 3, RGB(70, 70, 80));
    HPEN oldPen = (HPEN)SelectObject(hdc, trackPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, centerX - radius, centerY - radius, centerX + radius, centerY + radius);
    DeleteObject(trackPen);

    float rad = angleDeg * 3.14159265f / 180.0f;
    int dotX = centerX + static_cast<int>(std::cos(rad) * radius);
    int dotY = centerY + static_cast<int>(std::sin(rad) * radius);

    HBRUSH accentBrush = CreateSolidBrush(RGB(14, 165, 233));
    HPEN accentPen = CreatePen(PS_SOLID, 1, RGB(14, 165, 233));
    SelectObject(hdc, accentBrush);
    SelectObject(hdc, accentPen);
    Ellipse(hdc, dotX - 4, dotY - 4, dotX + 4, dotY + 4);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(accentBrush);
    DeleteObject(accentPen);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCCALCSIZE:
        if (wParam == TRUE)
            return 0; // Fixes "Half Background" DWM bug
        break;

    case WM_CREATE:
    {
        g_mainHwnd = hwnd;
        InitSettingsPath();
        LoadSettings();
        UpdateThemeColors();
        CleanupOldTempFiles(3);
        ApplyWindowBackdrop(hwnd, BackdropType::Acrylic);
        RegisterTrayDropTarget(hwnd);
        CalculateTargetSize(false);
        UpdateTrayWindowGeometry(hwnd);
        return 0;
    }

    case WM_DOWNLOAD_COMPLETE:
    {
        auto *res = reinterpret_cast<DownloadResult *>(lParam);
        if (res)
        {
            auto it = std::find_if(g_items.begin(), g_items.end(), [&](const PinnedItem &item)
                                   { return item.id == res->itemId; });
            if (it != g_items.end())
            {
                std::wstring cleanFinal = NormalizeWinPath(res->finalPath);
                if (res->success && GetFileAttributesW(cleanFinal.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    it->path = cleanFinal;
                    it->name = !res->displayName.empty() ? res->displayName : cleanFinal.substr(cleanFinal.find_last_of(L"\\/") + 1);
                    it->icon = GetIconOrThumbnail(cleanFinal, g_settings.largeIcons);
                    it->isLoading = false;
                }
                else
                {
                    g_items.erase(it);
                }
            }
            delete res;
            CalculateTargetSize(g_isExpanded);
            SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }

    case WM_KEYDOWN:
    {
        if (wParam == VK_DELETE)
        {
            DeleteSelectedItems(hwnd);
            return 0;
        }
        if (wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            HandleClipboardPaste(hwnd);
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

            if (std::fabs(diffW) > 0.5f || std::fabs(diffH) > 0.5f)
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
            if (std::fabs(diffScroll) > 0.2f)
            {
                g_scrollOffset += diffScroll * 0.25f;
                needsRedraw = true;
            }
            else
            {
                g_scrollOffset = g_targetScroll;
            }

            bool anyLoading = false;
            for (size_t i = 0; i < g_items.size(); ++i)
            {
                if (g_items[i].isLoading)
                    anyLoading = true;
                float targetScale = (static_cast<int>(i) == g_hoveredIndex && g_isExpanded && g_settings.enableMagnify) ? 1.22f : 1.0f;
                float diffScale = targetScale - g_items[i].currentScale;
                if (std::fabs(diffScale) > 0.01f)
                {
                    g_items[i].currentScale += diffScale * 0.28f;
                    needsRedraw = true;
                }
                else
                    g_items[i].currentScale = targetScale;
            }

            if (anyLoading)
            {
                g_SpinnerAngle = std::fmod(g_SpinnerAngle + 14.0f, 360.0f);
                needsRedraw = true;
            }

            if (!needsRedraw && g_curW == g_targetW && g_curH == g_targetH)
                KillTimer(hwnd, ANIM_TIMER_ID);

            UpdateTrayWindowGeometry(hwnd);
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
            g_targetScroll = std::max(0.0f, std::min(g_targetScroll, g_maxScroll));
            SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        // Removed the -16 coordinate hacks so hitboxes align perfectly
        int x = static_cast<short>(LOWORD(lParam));
        int y = static_cast<short>(HIWORD(lParam));

        if (g_isDraggingWindow && (wParam & MK_LBUTTON))
        {
            POINT pt;
            GetCursorPos(&pt);
            int screenW = GetSystemMetrics(SM_CXSCREEN), screenH = GetSystemMetrics(SM_CYSCREEN);
            if (g_settings.dockEdge == EDGE_TOP)
                g_settings.dockOffsetRatio = std::clamp(static_cast<float>(pt.x) / screenW, 0.1f, 0.9f);
            else
                g_settings.dockOffsetRatio = std::clamp(static_cast<float>(pt.y) / screenH, 0.1f, 0.9f);
            UpdateTrayWindowGeometry(hwnd);
            return 0;
        }

        if (!g_mouseTrackingActive)
        {
            TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g_mouseTrackingActive = true;
            TriggerExpand(hwnd);
        }

        POINT pt = {x, y};
        bool newSettingsHover = PtInRect(&g_settingsBtnRect, pt);
        if (newSettingsHover != g_settingsHovered)
        {
            g_settingsHovered = newSettingsHover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (g_isTrackingDrag && (wParam & MK_LBUTTON))
        {
            if (std::abs(x - g_dragStartPoint.x) > 6 || std::abs(y - g_dragStartPoint.y) > 6)
            {
                int dragIdx = g_dragCandidateIndex;
                g_isTrackingDrag = false;
                g_dragCandidateIndex = -1;
                StartDraggingSelectedOut(hwnd, dragIdx);
                return 0;
            }
        }

        int newHover = -1;
        if (!newSettingsHover)
        {
            for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
            {
                if (PtInRect(&g_items[i].rect, pt))
                {
                    newHover = i;
                    break;
                }
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
        g_isDraggingWindow = false;

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
        int x = static_cast<short>(LOWORD(lParam));
        int y = static_cast<short>(HIWORD(lParam));
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
                    int startIdx = std::min(g_lastClickedIndex, i);
                    int endIdx = std::max(g_lastClickedIndex, i);
                    if (!isCtrl)
                        for (auto &item : g_items)
                            item.isSelected = false;
                    for (int k = startIdx; k <= endIdx; ++k)
                        g_items[k].isSelected = true;
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
            g_isDraggingWindow = true;
            SetCapture(hwnd);
            for (auto &item : g_items)
                item.isSelected = false;
            g_lastClickedIndex = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
    {
        int x = static_cast<short>(LOWORD(lParam));
        int y = static_cast<short>(HIWORD(lParam));
        POINT pt = {x, y};

        for (const auto &item : g_items)
        {
            if (!item.isLoading && PtInRect(&item.rect, pt))
            {
                PruneMissingItems(hwnd);
                std::wstring cleanPath = NormalizeWinPath(item.path);
                ShellExecuteW(nullptr, L"open", cleanPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_isDraggingWindow)
        {
            g_isDraggingWindow = false;
            ReleaseCapture();
            SaveSettings();
        }
        g_isTrackingDrag = false;
        g_dragCandidateIndex = -1;
        return 0;

    case WM_RBUTTONUP:
    {
        int x = static_cast<short>(LOWORD(lParam));
        int y = static_cast<short>(HIWORD(lParam));
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
                    for (auto &item : g_items)
                        item.isSelected = false;
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

            POINT screenPt = {x, y};
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
                    if (item.isSelected && !item.isLoading)
                    {
                        std::wstring cleanPath = NormalizeWinPath(item.path);
                        ShellExecuteW(nullptr, L"open", cleanPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
            }
            else if (cmd == 1002 && !g_items[clickedIdx].isLoading)
            {
                std::wstring cleanPath = NormalizeWinPath(g_items[clickedIdx].path);
                std::wstring params = L"/select,\"" + cleanPath + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
            }
            else if (cmd == 1003)
                TogglePinSelectedItems(hwnd);
            else if (cmd == 1004)
                CompressSelectedToZip(hwnd);
            else if (cmd == 1005)
                DeleteSelectedItems(hwnd);
            return 0;
        }
        else
        {
            HMENU hEmptyMenu = CreatePopupMenu();
            AppendMenuW(hEmptyMenu, MF_STRING, 1010, L"Paste from Clipboard (Ctrl+V)");
            AppendMenuW(hEmptyMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hEmptyMenu, MF_STRING, 1011, L"Settings...");

            POINT screenPt = {x, y};
            ClientToScreen(hwnd, &screenPt);

            g_isMenuOpen = true;
            int cmd = TrackPopupMenu(hEmptyMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, hwnd, nullptr);
            DestroyMenu(hEmptyMenu);
            g_isMenuOpen = false;

            if (cmd == 1010)
                HandleClipboardPaste(hwnd);
            else if (cmd == 1011)
            {
                OpenSettingsDialog(hwnd);
                CalculateTargetSize(g_isExpanded);
                SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
            }
            return 0;
        }
    }

    // HOTKEY FIX: Hides the application entirely instead of just collapsing
    case WM_HOTKEY:
    {
        if (wParam == HOTKEY_TOGGLE_ID)
        {
            if (IsWindowVisible(hwnd))
            {
                ShowWindow(hwnd, SW_HIDE);
            }
            else
            {
                ShowWindow(hwnd, SW_SHOW);
                UpdateTrayWindowGeometry(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    // case WM_PAINT:
    // {
    //     PAINTSTRUCT ps;
    //     HDC hdc = BeginPaint(hwnd, &ps);

    //     RECT clientRc;
    //     GetClientRect(hwnd, &clientRc);
    //     int width = std::max(1, static_cast<int>(clientRc.right));
    //     int height = std::max(1, static_cast<int>(clientRc.bottom));

    //     HDC memDC = CreateCompatibleDC(hdc);
    //     HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
    //     HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

    //     HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
    //     FillRect(memDC, &clientRc, bgBrush);
    //     DeleteObject(bgBrush);

    //     HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
    //     HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    //     SelectObject(memDC, borderPen);
    //     SelectObject(memDC, nullBrush);

    //     // Exact 1:1 border drawing matching the window region clip without offsets
    //     if (g_settings.dockEdge == EDGE_TOP)
    //         RoundRect(memDC, 0, -14, width, height, CORNER_RADIUS, CORNER_RADIUS);
    //     else if (g_settings.dockEdge == EDGE_LEFT)
    //         RoundRect(memDC, -14, 0, width, height, CORNER_RADIUS, CORNER_RADIUS);
    //     else
    //         RoundRect(memDC, 0, 0, width + 14, height, CORNER_RADIUS, CORNER_RADIUS);
    //     DeleteObject(borderPen);

    //     bool isVert = (g_settings.dockEdge == EDGE_LEFT || g_settings.dockEdge == EDGE_RIGHT);

    //     if ((g_curH < 30.0f && !isVert) || (g_curW < 30.0f && isVert) || !g_isExpanded)
    //     {
    //         for (auto &item : g_items)
    //             item.rect = {0, 0, 0, 0};

    //         HBRUSH pillBrush = CreateSolidBrush(g_theme.pillHover);
    //         HPEN pillPen = CreatePen(PS_SOLID, 1, g_theme.pillBorder);
    //         SelectObject(memDC, pillBrush);
    //         SelectObject(memDC, pillPen);
    //         if (!isVert)
    //             RoundRect(memDC, (width - 36) / 2, 4, (width + 36) / 2, 8, 4, 4);
    //         else
    //             RoundRect(memDC, 4, (height - 36) / 2, 8, (height + 36) / 2, 4, 4);
    //         DeleteObject(pillBrush);
    //         DeleteObject(pillPen);
    //     }
    //     else
    //     {
    //         g_settingsBtnRect = isVert
    //                                 ? RECT{(width - 32) / 2, height - 38, (width + 32) / 2, height - 10}
    //                                 : RECT{width - 38, 12, width - 12, 38};

    //         if (g_settingsHovered)
    //         {
    //             HBRUSH gearHover = CreateSolidBrush(g_theme.pillHover);
    //             HPEN gearPen = CreatePen(PS_SOLID, 1, g_theme.pillBorder);
    //             SelectObject(memDC, gearHover);
    //             SelectObject(memDC, gearPen);
    //             RoundRect(memDC, g_settingsBtnRect.left, g_settingsBtnRect.top, g_settingsBtnRect.right, g_settingsBtnRect.bottom, 8, 8);
    //             DeleteObject(gearHover);
    //             DeleteObject(gearPen);
    //         }
    //         HFONT gearFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
    //         HFONT oldGFont = (HFONT)SelectObject(memDC, gearFont);
    //         SetBkMode(memDC, TRANSPARENT);
    //         SetTextColor(memDC, g_settingsHovered ? g_theme.textPrimary : g_theme.textSec);
    //         DrawTextW(memDC, L"\x2699", -1, &g_settingsBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    //         SelectObject(memDC, oldGFont);
    //         DeleteObject(gearFont);

    //         if (g_items.empty())
    //         {
    //             HFONT hFont = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    //             HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
    //             SetBkMode(memDC, TRANSPARENT);
    //             SetTextColor(memDC, g_theme.textSec);

    //             RECT textRc = {0, 0, width, height};
    //             DrawTextW(memDC, isVert ? L"Drop here" : L"Drop files, web media, or paste (Ctrl+V) here", -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    //             SelectObject(memDC, oldFont);
    //             DeleteObject(hFont);
    //         }
    //         else
    //         {
    //             int baseItemW = g_settings.largeIcons ? 68 : 60;
    //             int baseItemH = g_settings.largeIcons ? 76 : 68;
    //             int baseIconSize = g_settings.largeIcons ? 40 : 32;
    //             int itemSpacing = 8;
    //             int startY = 14;

    //             std::vector<int> pinnedIndices;
    //             std::vector<int> unpinnedIndices;
    //             for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
    //             {
    //                 if (g_items[i].isPinned)
    //                     pinnedIndices.push_back(i);
    //                 else
    //                     unpinnedIndices.push_back(i);
    //             }

    //             HFONT hFont = CreateFontW(g_settings.largeIcons ? -12 : -11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    //             HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
    //             SetBkMode(memDC, TRANSPARENT);

    //             int safeLimit = isVert ? (height - 46) : (width - 46);

    //             auto renderItemCard = [&](int idx, int drawCoord, bool showPinBadge, int clipStart, int clipEnd)
    //             {
    //                 float scale = g_items[idx].currentScale;
    //                 int itemW = static_cast<int>(baseItemW * scale);
    //                 int itemH = static_cast<int>(baseItemH * scale);
    //                 int shiftW = (itemW - baseItemW) / 2;
    //                 int shiftH = (itemH - baseItemH) / 2;

    //                 RECT pillRect;
    //                 if (isVert)
    //                 {
    //                     int cardX = (width - baseItemW) / 2;
    //                     pillRect = {cardX - shiftW, drawCoord - shiftH, cardX + baseItemW + shiftW, drawCoord + baseItemH + shiftH};
    //                 }
    //                 else
    //                 {
    //                     pillRect = {drawCoord - shiftW, startY - shiftH, drawCoord + baseItemW + shiftW, startY + baseItemH + shiftH};
    //                 }

    //                 RECT hitRect = pillRect;
    //                 if (isVert)
    //                 {
    //                     hitRect.top = std::max<LONG>(hitRect.top, clipStart);
    //                     hitRect.bottom = std::min<LONG>(hitRect.bottom, clipEnd);
    //                     if (hitRect.bottom <= hitRect.top)
    //                         hitRect = {0, 0, 0, 0};
    //                 }
    //                 else
    //                 {
    //                     hitRect.left = std::max<LONG>(hitRect.left, clipStart);
    //                     hitRect.right = std::min<LONG>(hitRect.right, clipEnd);
    //                     if (hitRect.right <= hitRect.left)
    //                         hitRect = {0, 0, 0, 0};
    //                 }
    //                 g_items[idx].rect = hitRect;

    //                 if (g_items[idx].isSelected)
    //                 {
    //                     HBRUSH selBrush = CreateSolidBrush(g_theme.selBg);
    //                     HPEN borderSel = CreatePen(PS_SOLID, 1, g_theme.selBorder);
    //                     SelectObject(memDC, selBrush);
    //                     SelectObject(memDC, borderSel);
    //                     RoundRect(memDC, pillRect.left, pillRect.top, pillRect.right, pillRect.bottom, 12, 12);
    //                     DeleteObject(selBrush);
    //                     DeleteObject(borderSel);
    //                 }
    //                 else if (idx == g_hoveredIndex)
    //                 {
    //                     HBRUSH hoverBrush = CreateSolidBrush(g_theme.pillHover);
    //                     HPEN borderPill = CreatePen(PS_SOLID, 1, g_theme.pillBorder);
    //                     SelectObject(memDC, hoverBrush);
    //                     SelectObject(memDC, borderPill);
    //                     RoundRect(memDC, pillRect.left, pillRect.top, pillRect.right, pillRect.bottom, 12, 12);
    //                     DeleteObject(hoverBrush);
    //                     DeleteObject(borderPill);
    //                 }

    //                 int iconSize = static_cast<int>(baseIconSize * scale);
    //                 int iconX = pillRect.left + (itemW - iconSize) / 2;
    //                 int iconY = pillRect.top + 6;

    //                 if (g_items[idx].isLoading)
    //                     DrawLoadingSpinner(memDC, iconX + iconSize / 2, iconY + iconSize / 2, iconSize / 2 - 2, g_SpinnerAngle);
    //                 else if (g_items[idx].icon)
    //                 {
    //                     DrawIconEx(memDC, iconX, iconY, g_items[idx].icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    //                     if (showPinBadge)
    //                     {
    //                         HBRUSH pinBadge = CreateSolidBrush(RGB(245, 158, 11));
    //                         HPEN pinRing = CreatePen(PS_SOLID, 1, RGB(180, 100, 10));
    //                         SelectObject(memDC, pinBadge);
    //                         SelectObject(memDC, pinRing);
    //                         RoundRect(memDC, iconX + iconSize - 8, iconY, iconX + iconSize + 2, iconY + 10, 3, 3);
    //                         DeleteObject(pinBadge);
    //                         DeleteObject(pinRing);
    //                     }
    //                 }

    //                 RECT textRc = {pillRect.left + 2, pillRect.bottom - 20, pillRect.right - 2, pillRect.bottom - 3};
    //                 SetTextColor(memDC, g_items[idx].isSelected ? g_theme.textPrimary : g_theme.textSec);
    //                 DrawTextW(memDC, g_items[idx].name.c_str(), -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_END_ELLIPSIS);

    //                 return isVert ? baseItemH : baseItemW;
    //             };

    //             int currentPos = 16;
    //             int stepSize = (isVert ? baseItemH : baseItemW) + itemSpacing;

    //             if (!pinnedIndices.empty())
    //             {
    //                 HRGN pinClip = isVert ? CreateRectRgn(0, 8, width, safeLimit) : CreateRectRgn(8, 0, safeLimit, height);
    //                 SelectClipRgn(memDC, pinClip);

    //                 for (int idx : pinnedIndices)
    //                 {
    //                     int sz = renderItemCard(idx, currentPos, true, 8, safeLimit);
    //                     currentPos += sz + itemSpacing;
    //                 }

    //                 SelectClipRgn(memDC, nullptr);
    //                 DeleteObject(pinClip);

    //                 if (!unpinnedIndices.empty() && currentPos < safeLimit - 20)
    //                 {
    //                     HPEN divPen = CreatePen(PS_SOLID, 1, g_theme.div);
    //                     HPEN oldPen = (HPEN)SelectObject(memDC, divPen);
    //                     if (!isVert)
    //                     {
    //                         MoveToEx(memDC, currentPos + 4, startY + 8, nullptr);
    //                         LineTo(memDC, currentPos + 4, startY + baseItemH - 8);
    //                     }
    //                     else
    //                     {
    //                         int cardX = (width - baseItemW) / 2;
    //                         MoveToEx(memDC, cardX + 8, currentPos + 4, nullptr);
    //                         LineTo(memDC, cardX + baseItemW - 8, currentPos + 4);
    //                     }
    //                     SelectObject(memDC, oldPen);
    //                     DeleteObject(divPen);
    //                     currentPos += 14;
    //                 }
    //             }

    //             if (!unpinnedIndices.empty())
    //             {
    //                 int unpinnedStart = currentPos;
    //                 int availableSpace = std::max(40, safeLimit - unpinnedStart);
    //                 int totalUnpinnedLen = static_cast<int>(unpinnedIndices.size()) * stepSize;
    //                 g_maxScroll = static_cast<float>(std::max(0, totalUnpinnedLen - availableSpace));
    //                 g_scrollOffset = std::min(g_scrollOffset, g_maxScroll);

    //                 HRGN clipRgn = isVert ? CreateRectRgn(0, unpinnedStart - 6, width, safeLimit) : CreateRectRgn(unpinnedStart - 6, 0, safeLimit, height);
    //                 SelectClipRgn(memDC, clipRgn);

    //                 int scrollCoord = unpinnedStart - static_cast<int>(std::lround(g_scrollOffset));
    //                 for (int idx : unpinnedIndices)
    //                 {
    //                     int sz = renderItemCard(idx, scrollCoord, false, unpinnedStart, safeLimit);
    //                     scrollCoord += sz + itemSpacing;
    //                 }

    //                 SelectClipRgn(memDC, nullptr);
    //                 DeleteObject(clipRgn);

    //                 if (g_maxScroll > 0.0f)
    //                 {
    //                     int trackLen = availableSpace;
    //                     int thumbLen = std::max(24, trackLen * availableSpace / std::max(1, totalUnpinnedLen));
    //                     int thumbOffset = static_cast<int>(std::lround((trackLen - thumbLen) * (g_scrollOffset / g_maxScroll)));

    //                     HBRUSH trackBrush = CreateSolidBrush(g_theme.scrollTrack);
    //                     HBRUSH thumbBrush = CreateSolidBrush(g_theme.scrollThumb);
    //                     HPEN noPen = CreatePen(PS_NULL, 0, 0);
    //                     SelectObject(memDC, noPen);

    //                     if (!isVert)
    //                     {
    //                         SelectObject(memDC, trackBrush);
    //                         RoundRect(memDC, unpinnedStart, startY + baseItemH + 8, unpinnedStart + trackLen, startY + baseItemH + 12, 2, 2);
    //                         SelectObject(memDC, thumbBrush);
    //                         RoundRect(memDC, unpinnedStart + thumbOffset, startY + baseItemH + 8, unpinnedStart + thumbOffset + thumbLen, startY + baseItemH + 12, 2, 2);
    //                     }
    //                     else
    //                     {
    //                         SelectObject(memDC, trackBrush);
    //                         RoundRect(memDC, width - 8, unpinnedStart, width - 4, unpinnedStart + trackLen, 2, 2);
    //                         SelectObject(memDC, thumbBrush);
    //                         RoundRect(memDC, width - 8, unpinnedStart + thumbOffset, width - 4, unpinnedStart + thumbOffset + thumbLen, 2, 2);
    //                     }
    //                     DeleteObject(trackBrush);
    //                     DeleteObject(thumbBrush);
    //                     DeleteObject(noPen);
    //                 }
    //             }
    //             else
    //             {
    //                 g_maxScroll = 0.0f;
    //             }

    //             if (g_hoveredIndex >= 0 && g_hoveredIndex < static_cast<int>(g_items.size()))
    //             {
    //                 const auto &item = g_items[g_hoveredIndex];
    //                 if (item.rect.right > item.rect.left && item.rect.bottom > item.rect.top)
    //                 {
    //                     HFONT tipFont = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    //                     HFONT oldTipFont = (HFONT)SelectObject(memDC, tipFont);

    //                     RECT calcRc = {0, 0, 0, 0};
    //                     DrawTextW(memDC, item.name.c_str(), -1, &calcRc, DT_CALCRECT | DT_SINGLELINE);
    //                     int tipWidth = std::min(width - 12, static_cast<int>(calcRc.right - calcRc.left) + 20);
    //                     int tipHeight = 22;

    //                     int tipX, tipY;
    //                     if (!isVert)
    //                     {
    //                         tipX = item.rect.left + ((item.rect.right - item.rect.left) - tipWidth) / 2;
    //                         tipX = std::max(8, std::min(tipX, width - tipWidth - 8));
    //                         tipY = height - 26; // perfectly clears the bottom boundary
    //                     }
    //                     else
    //                     {
    //                         tipX = (width - tipWidth) / 2;
    //                         tipY = std::min(safeLimit - tipHeight - 2, static_cast<int>(item.rect.bottom) + 2);
    //                     }

    //                     HBRUSH tipBg = CreateSolidBrush(g_theme.tipBg);
    //                     HPEN tipBorder = CreatePen(PS_SOLID, 1, g_theme.border);
    //                     SelectObject(memDC, tipBg);
    //                     SelectObject(memDC, tipBorder);
    //                     RoundRect(memDC, tipX, tipY, tipX + tipWidth, tipY + tipHeight, 10, 10);
    //                     DeleteObject(tipBg);
    //                     DeleteObject(tipBorder);

    //                     RECT labelRc = {tipX + 4, tipY, tipX + tipWidth - 4, tipY + tipHeight};
    //                     SetTextColor(memDC, RGB(255, 255, 255));
    //                     DrawTextW(memDC, item.name.c_str(), -1, &labelRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
    //                     SelectObject(memDC, oldTipFont);
    //                     DeleteObject(tipFont);
    //                 }
    //             }

    //             SelectObject(memDC, oldFont);
    //             DeleteObject(hFont);
    //         }
    //     }

    //     BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
    //     SelectObject(memDC, oldBitmap);
    //     DeleteObject(memBitmap);
    //     DeleteDC(memDC);
    //     EndPaint(hwnd, &ps);
    //     return 0;
    // }


    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        int width = std::max(1, static_cast<int>(clientRc.right));
        int height = std::max(1, static_cast<int>(clientRc.bottom));

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
        HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

        HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &clientRc, bgBrush);
        DeleteObject(bgBrush);

        HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
        HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
        SelectObject(memDC, borderPen);
        SelectObject(memDC, nullBrush);

        if (g_settings.dockEdge == EDGE_TOP)
            RoundRect(memDC, 0, -14, width, height, CORNER_RADIUS, CORNER_RADIUS);
        else if (g_settings.dockEdge == EDGE_LEFT)
            RoundRect(memDC, -14, 0, width, height, CORNER_RADIUS, CORNER_RADIUS);
        else
            RoundRect(memDC, 0, 0, width + 14, height, CORNER_RADIUS, CORNER_RADIUS);
        DeleteObject(borderPen);

        bool isVert = (g_settings.dockEdge == EDGE_LEFT || g_settings.dockEdge == EDGE_RIGHT);

        if ((g_curH < 30.0f && !isVert) || (g_curW < 30.0f && isVert) || !g_isExpanded)
        {
            for (auto &item : g_items) item.rect = {0, 0, 0, 0};

            HBRUSH pillBrush = CreateSolidBrush(g_theme.pillHover);
            HPEN pillPen = CreatePen(PS_SOLID, 1, g_theme.pillBorder);
            SelectObject(memDC, pillBrush);
            SelectObject(memDC, pillPen);
            if (!isVert) RoundRect(memDC, (width - 36) / 2, 4, (width + 36) / 2, 8, 4, 4);
            else RoundRect(memDC, 4, (height - 36) / 2, 8, (height + 36) / 2, 4, 4);
            DeleteObject(pillBrush);
            DeleteObject(pillPen);
        }
        else
        {
            g_settingsBtnRect = isVert
                                    ? RECT{(width - 32) / 2, height - 38, (width + 32) / 2, height - 10}
                                    : RECT{width - 38, 12, width - 12, 38};

            if (g_settingsHovered)
            {
                HBRUSH gearHover = CreateSolidBrush(g_theme.pillHover);
                HPEN gearPen = CreatePen(PS_SOLID, 1, g_theme.pillBorder);
                SelectObject(memDC, gearHover);
                SelectObject(memDC, gearPen);
                RoundRect(memDC, g_settingsBtnRect.left, g_settingsBtnRect.top, g_settingsBtnRect.right, g_settingsBtnRect.bottom, 8, 8);
                DeleteObject(gearHover);
                DeleteObject(gearPen);
            }
            HFONT gearFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
            HFONT oldGFont = (HFONT)SelectObject(memDC, gearFont);
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, g_settingsHovered ? g_theme.textPrimary : g_theme.textSec);
            DrawTextW(memDC, L"\x2699", -1, &g_settingsBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            SelectObject(memDC, oldGFont);
            DeleteObject(gearFont);

            if (g_items.empty())
            {
                HFONT hFont = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, g_theme.textPrimary);

                RECT textRc = {0, 0, width, height};
                DrawTextW(memDC, isVert ? L"Drop here" : L"Drop files, web media, or paste (Ctrl+V) here", -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
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
                    if (g_items[i].isPinned) pinnedIndices.push_back(i);
                    else unpinnedIndices.push_back(i);
                }

                HFONT hFont = CreateFontW(g_settings.largeIcons ? -12 : -11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
                SetBkMode(memDC, TRANSPARENT);

                int safeLimit = isVert ? (height - 46) : (width - 46);

                auto renderItemCard = [&](int idx, int drawCoord, bool showPinBadge, int clipStart, int clipEnd)
                {
                    float scale = g_items[idx].currentScale;
                    int itemW = static_cast<int>(baseItemW * scale);
                    int itemH = static_cast<int>(baseItemH * scale);
                    int shiftW = (itemW - baseItemW) / 2;
                    int shiftH = (itemH - baseItemH) / 2;

                    RECT pillRect;
                    if (isVert)
                    {
                        int cardX = (width - baseItemW) / 2;
                        pillRect = {cardX - shiftW, drawCoord - shiftH, cardX + baseItemW + shiftW, drawCoord + baseItemH + shiftH};
                    }
                    else
                    {
                        pillRect = {drawCoord - shiftW, startY - shiftH, drawCoord + baseItemW + shiftW, startY + baseItemH + shiftH};
                    }

                    RECT hitRect = pillRect;
                    if (isVert)
                    {
                        hitRect.top = std::max<LONG>(hitRect.top, clipStart);
                        hitRect.bottom = std::min<LONG>(hitRect.bottom, clipEnd);
                        if (hitRect.bottom <= hitRect.top) hitRect = {0, 0, 0, 0};
                    }
                    else
                    {
                        hitRect.left = std::max<LONG>(hitRect.left, clipStart);
                        hitRect.right = std::min<LONG>(hitRect.right, clipEnd);
                        if (hitRect.right <= hitRect.left) hitRect = {0, 0, 0, 0};
                    }
                    g_items[idx].rect = hitRect;

                    if (g_items[idx].isSelected)
                    {
                        HBRUSH selBrush = CreateSolidBrush(g_theme.selBg);
                        HPEN borderSel = CreatePen(PS_SOLID, 1, g_theme.selBorder);
                        SelectObject(memDC, selBrush);
                        SelectObject(memDC, borderSel);
                        RoundRect(memDC, pillRect.left, pillRect.top, pillRect.right, pillRect.bottom, 12, 12);
                        DeleteObject(selBrush);
                        DeleteObject(borderSel);
                    }
                    else if (idx == g_hoveredIndex)
                    {
                        HBRUSH hoverBrush = CreateSolidBrush(g_theme.pillHover);
                        HPEN borderPill = CreatePen(PS_SOLID, 1, g_theme.pillBorder);
                        SelectObject(memDC, hoverBrush);
                        SelectObject(memDC, borderPill);
                        RoundRect(memDC, pillRect.left, pillRect.top, pillRect.right, pillRect.bottom, 12, 12);
                        DeleteObject(hoverBrush);
                        DeleteObject(borderPill);
                    }

                    int iconSize = static_cast<int>(baseIconSize * scale);
                    int iconX = pillRect.left + (itemW - iconSize) / 2;
                    int iconY = pillRect.top + 6;

                    if (g_items[idx].isLoading)
                        DrawLoadingSpinner(memDC, iconX + iconSize / 2, iconY + iconSize / 2, iconSize / 2 - 2, g_SpinnerAngle);
                    else if (g_items[idx].icon)
                    {
                        DrawIconEx(memDC, iconX, iconY, g_items[idx].icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
                        if (showPinBadge)
                        {
                            HBRUSH pinBadge = CreateSolidBrush(RGB(245, 158, 11));
                            HPEN pinRing = CreatePen(PS_SOLID, 1, RGB(180, 100, 10));
                            SelectObject(memDC, pinBadge);
                            SelectObject(memDC, pinRing);
                            RoundRect(memDC, iconX + iconSize - 8, iconY, iconX + iconSize + 2, iconY + 10, 3, 3);
                            DeleteObject(pinBadge);
                            DeleteObject(pinRing);
                        }
                    }

                    RECT textRc = {pillRect.left + 2, pillRect.bottom - 20, pillRect.right - 2, pillRect.bottom - 3};
                    
                    // NEW: Draw a subtle mini-pill behind the label so DWM glass doesn't make dark text transparent
                    if (!g_items[idx].isSelected && idx != g_hoveredIndex)
                    {
                        HBRUSH labelBg = CreateSolidBrush(g_theme.tipBg);
                        HPEN noPen = CreatePen(PS_NULL, 0, 0);
                        HGDIOBJ oldB = SelectObject(memDC, labelBg);
                        HGDIOBJ oldP = SelectObject(memDC, noPen);

                        RECT calcRc = {0, 0, 0, 0};
                        DrawTextW(memDC, g_items[idx].name.c_str(), -1, &calcRc, DT_CALCRECT | DT_SINGLELINE);
                        
                        // Calculate a perfectly fitted, padded mini-pill
                        int textW = std::min<int>(calcRc.right - calcRc.left + 12, textRc.right - textRc.left);
                        int textX = textRc.left + ((textRc.right - textRc.left) - textW) / 2;
                        
                        RoundRect(memDC, textX, textRc.top - 1, textX + textW, textRc.bottom + 1, 8, 8);

                        SelectObject(memDC, oldB);
                        SelectObject(memDC, oldP);
                        DeleteObject(labelBg);
                        DeleteObject(noPen);
                    }

                    SetTextColor(memDC, g_items[idx].isSelected ? RGB(255, 255, 255) : g_theme.textPrimary);
                    DrawTextW(memDC, g_items[idx].name.c_str(), -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_END_ELLIPSIS);

                    return isVert ? baseItemH : baseItemW;
                };

                int currentPos = 16;
                int stepSize = (isVert ? baseItemH : baseItemW) + itemSpacing;

                if (!pinnedIndices.empty())
                {
                    HRGN pinClip = isVert ? CreateRectRgn(0, 8, width, safeLimit) : CreateRectRgn(8, 0, safeLimit, height);
                    SelectClipRgn(memDC, pinClip);

                    for (int idx : pinnedIndices)
                    {
                        int sz = renderItemCard(idx, currentPos, true, 8, safeLimit);
                        currentPos += sz + itemSpacing;
                    }

                    SelectClipRgn(memDC, nullptr);
                    DeleteObject(pinClip);

                    if (!unpinnedIndices.empty() && currentPos < safeLimit - 20)
                    {
                        HPEN divPen = CreatePen(PS_SOLID, 1, g_theme.div);
                        HPEN oldPen = (HPEN)SelectObject(memDC, divPen);
                        if (!isVert)
                        {
                            MoveToEx(memDC, currentPos + 4, startY + 8, nullptr);
                            LineTo(memDC, currentPos + 4, startY + baseItemH - 8);
                        }
                        else
                        {
                            int cardX = (width - baseItemW) / 2;
                            MoveToEx(memDC, cardX + 8, currentPos + 4, nullptr);
                            LineTo(memDC, cardX + baseItemW - 8, currentPos + 4);
                        }
                        SelectObject(memDC, oldPen);
                        DeleteObject(divPen);
                        currentPos += 14;
                    }
                }

                if (!unpinnedIndices.empty())
                {
                    int unpinnedStart = currentPos;
                    int availableSpace = std::max(40, safeLimit - unpinnedStart);
                    int totalUnpinnedLen = static_cast<int>(unpinnedIndices.size()) * stepSize;
                    g_maxScroll = static_cast<float>(std::max(0, totalUnpinnedLen - availableSpace));
                    g_scrollOffset = std::min(g_scrollOffset, g_maxScroll);

                    HRGN clipRgn = isVert ? CreateRectRgn(0, unpinnedStart - 6, width, safeLimit) : CreateRectRgn(unpinnedStart - 6, 0, safeLimit, height);
                    SelectClipRgn(memDC, clipRgn);

                    int scrollCoord = unpinnedStart - static_cast<int>(std::lround(g_scrollOffset));
                    for (int idx : unpinnedIndices)
                    {
                        int sz = renderItemCard(idx, scrollCoord, false, unpinnedStart, safeLimit);
                        scrollCoord += sz + itemSpacing;
                    }

                    SelectClipRgn(memDC, nullptr);
                    DeleteObject(clipRgn);

                    if (g_maxScroll > 0.0f)
                    {
                        int trackLen = availableSpace;
                        int thumbLen = std::max(24, trackLen * availableSpace / std::max(1, totalUnpinnedLen));
                        int thumbOffset = static_cast<int>(std::lround((trackLen - thumbLen) * (g_scrollOffset / g_maxScroll)));

                        HBRUSH trackBrush = CreateSolidBrush(g_theme.scrollTrack);
                        HBRUSH thumbBrush = CreateSolidBrush(g_theme.scrollThumb);
                        HPEN noPen = CreatePen(PS_NULL, 0, 0);
                        SelectObject(memDC, noPen);

                        if (!isVert)
                        {
                            SelectObject(memDC, trackBrush);
                            RoundRect(memDC, unpinnedStart, startY + baseItemH + 8, unpinnedStart + trackLen, startY + baseItemH + 12, 2, 2);
                            SelectObject(memDC, thumbBrush);
                            RoundRect(memDC, unpinnedStart + thumbOffset, startY + baseItemH + 8, unpinnedStart + thumbOffset + thumbLen, startY + baseItemH + 12, 2, 2);
                        }
                        else
                        {
                            SelectObject(memDC, trackBrush);
                            RoundRect(memDC, width - 8, unpinnedStart, width - 4, unpinnedStart + trackLen, 2, 2);
                            SelectObject(memDC, thumbBrush);
                            RoundRect(memDC, width - 8, unpinnedStart + thumbOffset, width - 4, unpinnedStart + thumbOffset + thumbLen, 2, 2);
                        }
                        DeleteObject(trackBrush);
                        DeleteObject(thumbBrush);
                        DeleteObject(noPen);
                    }
                }
                else
                {
                    g_maxScroll = 0.0f;
                }

                if (g_hoveredIndex >= 0 && g_hoveredIndex < static_cast<int>(g_items.size()))
                {
                    const auto &item = g_items[g_hoveredIndex];
                    if (item.rect.right > item.rect.left && item.rect.bottom > item.rect.top)
                    {
                        HFONT tipFont = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                        HFONT oldTipFont = (HFONT)SelectObject(memDC, tipFont);

                        RECT calcRc = {0, 0, 0, 0};
                        DrawTextW(memDC, item.name.c_str(), -1, &calcRc, DT_CALCRECT | DT_SINGLELINE);
                        int tipWidth = std::min(width - 12, static_cast<int>(calcRc.right - calcRc.left) + 20);
                        int tipHeight = 22;

                        int tipX, tipY;
                        if (!isVert)
                        {
                            tipX = item.rect.left + ((item.rect.right - item.rect.left) - tipWidth) / 2;
                            tipX = std::max(8, std::min(tipX, width - tipWidth - 8));
                            tipY = height - 26;
                        }
                        else
                        {
                            tipX = (width - tipWidth) / 2;
                            tipY = std::min(safeLimit - tipHeight - 2, static_cast<int>(item.rect.bottom) + 2);
                        }

                        HBRUSH tipBg = CreateSolidBrush(g_theme.tipBg);
                        HPEN tipBorder = CreatePen(PS_SOLID, 1, g_theme.border);
                        SelectObject(memDC, tipBg);
                        SelectObject(memDC, tipBorder);
                        RoundRect(memDC, tipX, tipY, tipX + tipWidth, tipY + tipHeight, 10, 10);
                        DeleteObject(tipBg);
                        DeleteObject(tipBorder);

                        RECT labelRc = {tipX + 4, tipY, tipX + tipWidth - 4, tipY + tipHeight};
                        // FIXED: Replaced hardcoded white with g_theme.textPrimary
                        SetTextColor(memDC, g_theme.textPrimary);
                        DrawTextW(memDC, item.name.c_str(), -1, &labelRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
                        SelectObject(memDC, oldTipFont);
                        DeleteObject(tipFont);
                    }
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
        RevokeTrayDropTarget(hwnd);
        KillTimer(hwnd, ANIM_TIMER_ID);
        for (auto &item : g_items)
            if (item.icon)
                DestroyIcon(item.icon);
        PostQuitMessage(0);
        return 0;

    case WM_SETTINGCHANGE:
    {
        if (lParam && _wcsicmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0)
        {
            if (g_settings.themeMode == 0)
            { // If set to System
                UpdateThemeColors();
                ApplyWindowBackdrop(hwnd, BackdropType::Acrylic);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        return 0;
    }
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
    
    // CRITICAL FIX: Set default background to Black so DWM makes it glass instantly on launch
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    RegisterClass(&wc);
    InitSettingsPath();
    LoadSettings();

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME, L"WinTray",
        WS_POPUP | WS_VISIBLE,
        100, 100, static_cast<int>(COLLAPSED_LENGTH), static_cast<int>(COLLAPSED_THICKNESS),
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
        return 0;

    ApplyGlobalHotkey(hwnd);
    UpdateTrayWindowGeometry(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterHotKey(hwnd, HOTKEY_TOGGLE_ID);
    OleUninitialize();
    return 0;
}