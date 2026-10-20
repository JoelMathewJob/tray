#include "state.h"
#include "settings.h"
#include "updater.h"
#include "media.h"
#include "blur.h"

#include <shellapi.h>
#include <shlobj.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>

// Global State Definitions
HWND g_mainHwnd = nullptr;
AppSettings g_settings;
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

static RECT g_settingsBtnRect = {};
static bool g_settingsHovered = false;

static float g_curW = COLLAPSED_W;
static float g_curH = COLLAPSED_H;
static float g_targetW = COLLAPSED_W;
static float g_targetH = COLLAPSED_H;

static float g_scrollOffset = 0.0f;
static float g_targetScroll = 0.0f;
static float g_maxScroll = 0.0f;

// -------------------------------------------------------------
// Core Tray Item & Sizing Helpers
// -------------------------------------------------------------
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
    if (expanded)
    {
        int pinnedCount = 0;
        int unpinnedCount = 0;
        for (const auto &item : g_items)
        {
            if (item.isPinned) pinnedCount++;
            else unpinnedCount++;
        }

        int itemWidth = g_settings.largeIcons ? 74 : 64;
        int pinnedW = pinnedCount * itemWidth;
        int unpinnedW = unpinnedCount * itemWidth;
        int basePadding = 90 + ((pinnedCount > 0 && unpinnedCount > 0) ? 30 : 0);

        int contentWidth = std::max(460, pinnedW + unpinnedW + basePadding);
        g_targetW = static_cast<float>(std::min(contentWidth, 980));
        g_targetH = g_settings.largeIcons ? 136.0f : 124.0f;
    }
    else
    {
        g_targetW = COLLAPSED_W;
        g_targetH = COLLAPSED_H;
    }
}

void PruneMissingItems(HWND hwnd)
{
    bool changed = false;
    auto it = g_items.begin();
    while (it != g_items.end())
    {
        if (!it->isLoading && GetFileAttributesW(it->path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            if (it->icon) DestroyIcon(it->icon);
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

int AddPlaceholderItem(const std::wstring& tempName)
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

void AddFileToTray(const std::wstring &filePath, bool isPinned)
{
    for (const auto &item : g_items)
    {
        if (!item.isLoading && _wcsicmp(item.path.c_str(), filePath.c_str()) == 0)
            return;
    }

    SHFILEINFOW sfi = {};
    SHGetFileInfoW(filePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_DISPLAYNAME);

    PinnedItem item;
    item.id = g_nextItemId++;
    item.path = filePath;
    item.name = (sfi.szDisplayName[0] != L'\0') ? sfi.szDisplayName : filePath.substr(filePath.find_last_of(L"\\/") + 1);
    item.icon = sfi.hIcon;
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
            if (it->icon) DestroyIcon(it->icon);
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
        if (item.isSelected) item.isPinned = hasUnpinned;
    }
    CalculateTargetSize(g_isExpanded);
    SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Helper to draw a spinning loader indicator for downloading items
void DrawLoadingSpinner(HDC hdc, int centerX, int centerY, int radius, float angleDeg)
{
    HPEN trackPen = CreatePen(PS_SOLID, 3, RGB(55, 55, 68));
    HPEN oldPen = (HPEN)SelectObject(hdc, trackPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, centerX - radius, centerY - radius, centerX + radius, centerY + radius);
    DeleteObject(trackPen);

    // Draw orbiting active dot on the ring
    float rad = angleDeg * 3.14159265f / 180.0f;
    int dotX = centerX + static_cast<int>(std::cos(rad) * radius);
    int dotY = centerY + static_cast<int>(std::sin(rad) * radius);

    HBRUSH accentBrush = CreateSolidBrush(RGB(56, 189, 248));
    HPEN accentPen = CreatePen(PS_SOLID, 1, RGB(56, 189, 248));
    SelectObject(hdc, accentBrush);
    SelectObject(hdc, accentPen);
    Ellipse(hdc, dotX - 4, dotY - 4, dotX + 4, dotY + 4);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(accentBrush);
    DeleteObject(accentPen);
}

// -------------------------------------------------------------
// Main Window Procedure
// -------------------------------------------------------------
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_mainHwnd = hwnd;
        InitSettingsPath();
        LoadSettings();
        ApplyWindowBackdrop(hwnd, BackdropType::Acrylic);
        RegisterTrayDropTarget(hwnd);
        CalculateTargetSize(false);

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int posX = (screenWidth - static_cast<int>(COLLAPSED_W)) / 2;
        SetWindowPos(hwnd, HWND_TOPMOST, posX, 0, static_cast<int>(COLLAPSED_W), static_cast<int>(COLLAPSED_H), SWP_NOACTIVATE);
        return 0;
    }

    case WM_DOWNLOAD_COMPLETE:
    {
        auto* res = reinterpret_cast<DownloadResult*>(lParam);
        if (res)
        {
            auto it = std::find_if(g_items.begin(), g_items.end(), [&](const PinnedItem& item) {
                return item.id == res->itemId;
            });

            if (it != g_items.end())
            {
                if (res->success && GetFileAttributesW(res->finalPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    SHFILEINFOW sfi = {};
                    SHGetFileInfoW(res->finalPath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_DISPLAYNAME);
                    it->path = res->finalPath;
                    it->name = !res->displayName.empty() ? res->displayName : sfi.szDisplayName;
                    it->icon = sfi.hIcon;
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
                if (g_items[i].isLoading) anyLoading = true;
                float targetScale = (static_cast<int>(i) == g_hoveredIndex && g_isExpanded && g_settings.enableMagnify) ? 1.25f : 1.0f;
                float diffScale = targetScale - g_items[i].currentScale;
                if (std::fabs(diffScale) > 0.01f)
                {
                    g_items[i].currentScale += diffScale * 0.28f;
                    needsRedraw = true;
                }
                else
                {
                    g_items[i].currentScale = targetScale;
                }
            }

            if (anyLoading)
            {
                g_SpinnerAngle = std::fmod(g_SpinnerAngle + 14.0f, 360.0f);
                needsRedraw = true;
            }

            if (!needsRedraw && g_curW == g_targetW && g_curH == g_targetH)
            {
                KillTimer(hwnd, ANIM_TIMER_ID);
            }

            int w = static_cast<int>(std::lround(g_curW));
            int h = static_cast<int>(std::lround(g_curH));
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
            g_targetScroll = std::max(0.0f, std::min(g_targetScroll, g_maxScroll));
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
            TriggerExpand(hwnd);
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
                    int startIdx = std::min(g_lastClickedIndex, i);
                    int endIdx = std::max(g_lastClickedIndex, i);
                    if (!isCtrl) for (auto &item : g_items) item.isSelected = false;
                    for (int k = startIdx; k <= endIdx; ++k) g_items[k].isSelected = true;
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
                        for (auto &item : g_items) item.isSelected = false;
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
            for (auto &item : g_items) item.isSelected = false;
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
            if (!item.isLoading && PtInRect(&item.rect, pt))
            {
                PruneMissingItems(hwnd);
                ShellExecuteW(nullptr, L"open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        g_isTrackingDrag = false;
        g_dragCandidateIndex = -1;
        return 0;

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
                    for (auto &item : g_items) item.isSelected = false;
                }
                g_items[clickedIdx].isSelected = true;
                g_lastClickedIndex = clickedIdx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            int selectedCount = 0;
            for (const auto &it : g_items)
                if (it.isSelected) selectedCount++;

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
                    if (item.isSelected && !item.isLoading)
                        ShellExecuteW(nullptr, L"open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            else if (cmd == 1002 && !g_items[clickedIdx].isLoading)
            {
                std::wstring params = L"/select,\"" + g_items[clickedIdx].path + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
            }
            else if (cmd == 1003) TogglePinSelectedItems(hwnd);
            else if (cmd == 1004) CompressSelectedToZip(hwnd);
            else if (cmd == 1005) DeleteSelectedItems(hwnd);
            return 0;
        }
        else
        {
            HMENU hEmptyMenu = CreatePopupMenu();
            AppendMenuW(hEmptyMenu, MF_STRING, 1010, L"Paste from Clipboard (Ctrl+V)");
            AppendMenuW(hEmptyMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hEmptyMenu, MF_STRING, 1011, L"Settings...");

            POINT screenPt = pt;
            ClientToScreen(hwnd, &screenPt);

            g_isMenuOpen = true;
            int cmd = TrackPopupMenu(hEmptyMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, hwnd, nullptr);
            DestroyMenu(hEmptyMenu);
            g_isMenuOpen = false;

            if (cmd == 1010) HandleClipboardPaste(hwnd);
            else if (cmd == 1011)
            {
                OpenSettingsDialog(hwnd);
                CalculateTargetSize(g_isExpanded);
                SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
            }
            return 0;
        }
    }

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
                int w = static_cast<int>(std::lround(g_curW));
                int h = static_cast<int>(std::lround(g_curH));
                HRGN hRgn = CreateRoundRectRgn(0, -14, w + 1, h + 1, CORNER_RADIUS, CORNER_RADIUS);
                SetWindowRgn(hwnd, hRgn, TRUE);
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

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
                DrawTextW(memDC, L"Drop files, web media, or paste (Ctrl+V) here", -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

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

                int currentX = 18;

                HFONT hFont = CreateFontW(g_settings.largeIcons ? -12 : -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
                SetBkMode(memDC, TRANSPARENT);

                auto renderItemCard = [&](int idx, int drawX, bool showPinBadge) {
                    float scale = g_items[idx].currentScale;
                    int itemW = static_cast<int>(baseItemW * scale);
                    int itemH = static_cast<int>(baseItemH * scale);
                    int shiftY = (itemH - baseItemH) / 2;

                    RECT pillRect = {drawX, startY - shiftY, drawX + itemW, startY + baseItemH + shiftY};
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

                    int iconSize = static_cast<int>(baseIconSize * scale);
                    int iconX = drawX + (itemW - iconSize) / 2;
                    int iconY = startY + 6 - shiftY;

                    if (g_items[idx].isLoading)
                    {
                        DrawLoadingSpinner(memDC, iconX + iconSize / 2, iconY + iconSize / 2, iconSize / 2 - 2, g_SpinnerAngle);
                    }
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

                    RECT textRc = {drawX + 2, startY + (g_settings.largeIcons ? 50 : 44) + shiftY, drawX + itemW - 2, startY + baseItemH - 6 + shiftY};
                    SetTextColor(memDC, g_items[idx].isSelected ? RGB(255, 255, 255) : RGB(195, 195, 205));
                    DrawTextW(memDC, g_items[idx].name.c_str(), -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_END_ELLIPSIS);
                    return itemW;
                };

                // --- SECTION 1: PINNED ITEMS ---
                if (!pinnedIndices.empty())
                {
                    for (int idx : pinnedIndices)
                    {
                        int itemW = renderItemCard(idx, currentX, true);
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
                    int availableWidth = width - unpinnedStartX - 48;
                    int totalUnpinnedW = static_cast<int>(unpinnedIndices.size()) * (baseItemW + itemSpacing);
                    g_maxScroll = static_cast<float>(std::max(0, totalUnpinnedW - availableWidth));

                    HRGN clipRgn = CreateRectRgn(unpinnedStartX - 4, startY - 8, width - 44, startY + baseItemH + 14);
                    SelectClipRgn(memDC, clipRgn);

                    int scrollX = unpinnedStartX - static_cast<int>(std::lround(g_scrollOffset));
                    for (int idx : unpinnedIndices)
                    {
                        int itemW = renderItemCard(idx, scrollX, false);
                        scrollX += itemW + itemSpacing;
                    }

                    SelectClipRgn(memDC, nullptr);
                    DeleteObject(clipRgn);

                    if (g_maxScroll > 0.0f)
                    {
                        int trackW = availableWidth;
                        int thumbW = std::max(22, trackW * availableWidth / totalUnpinnedW);
                        int thumbX = unpinnedStartX + static_cast<int>(std::lround((trackW - thumbW) * (g_scrollOffset / g_maxScroll)));

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
                    tipX = std::max(12, std::min(tipX, width - tipWidth - 12));
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
        RevokeTrayDropTarget(hwnd);
        KillTimer(hwnd, ANIM_TIMER_ID);
        for (auto &item : g_items)
        {
            if (item.icon) DestroyIcon(item.icon);
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

    BOOL registered = RegisterHotKey(hwnd, HOTKEY_TOGGLE_ID, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'T');
    if (!registered)
    {
        RegisterHotKey(hwnd, HOTKEY_TOGGLE_ID, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'T');
    }

    HRGN hRgn = CreateRoundRectRgn(0, -14, static_cast<int>(COLLAPSED_W) + 1, static_cast<int>(COLLAPSED_H) + 1, CORNER_RADIUS, CORNER_RADIUS);
    SetWindowRgn(hwnd, hRgn, TRUE);

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