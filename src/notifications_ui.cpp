#include "notifications_ui.h"
#include "state.h"
#include "actions.h"
#include "blur.h"
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>

std::vector<NotificationItem> g_notifications;
std::mutex g_notificationsMutex;

static void CheckAndExpireNotifications() {
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_notificationsMutex);
    for (auto& item : g_notifications) {
        if (item.isPending && item.expiresAt > 0 && now >= item.expiresAt) {
            item.isPending = false;
            item.isExpired = true;
            item.statusText = L"Expired (Timeout)";
            if (item.decision) {
                item.decision->store(3); // 3 = expired
            }
        }
    }
}

int GetPendingNotificationCount() {
    CheckAndExpireNotifications();
    std::lock_guard<std::mutex> lock(g_notificationsMutex);
    int count = 0;
    for (const auto& item : g_notifications) {
        if (item.isPending) count++;
    }
    return count;
}

void AddTransferNotification(const std::string& id, const std::wstring& senderName, const std::string& senderIp,
                             const std::wstring& fileName, ULONGLONG fileSize,
                             std::shared_ptr<std::atomic<int>> decision, int fileCount) {
    NotificationItem item;
    item.id = id;
    item.type = NotificationType::AIRDROP_REQUEST;
    item.senderName = senderName;
    item.senderIp = senderIp;
    item.fileName = fileName;
    item.fileCount = fileCount;
    item.fileSize = fileSize;
    item.timestamp = GetTickCount64();
    item.expiresAt = item.timestamp + 60000; // 1-minute expiration
    item.isPending = true;
    item.decision = decision;

    {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        g_notifications.insert(g_notifications.begin(), item);
        // Keep at most 20 recent notifications
        if (g_notifications.size() > 20) {
            g_notifications.pop_back();
        }
    }

    // Alert user with action toast & sound chime
    std::wstring notifMsg = (fileCount > 1) 
        ? (L"AirDrop (" + std::to_wstring(fileCount) + L" files) from " + senderName)
        : (L"AirDrop: " + senderName);
    ShowActionToast(notifMsg, L"\xE701", RGB(0, 160, 255));
    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

void AcceptNotification(const std::string& id) {
    std::wstring fName;
    std::wstring sName;
    {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        for (auto& item : g_notifications) {
            if (item.id == id && item.isPending) {
                if (item.decision) {
                    item.decision->store(1); // 1 = accepted
                }
                item.isPending = false;
                item.isAccepted = true;
                item.statusText = L"Accepted";
                fName = item.fileName;
                sName = item.senderName;
                break;
            }
        }
    }
    if (!fName.empty()) {
        ShowActionToast(L"Accepted file from " + sName, L"\xE73E", RGB(52, 199, 89));
    }
    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

void DeclineNotification(const std::string& id) {
    std::wstring fName;
    std::wstring sName;
    {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        for (auto& item : g_notifications) {
            if (item.id == id && item.isPending) {
                if (item.decision) {
                    item.decision->store(2); // 2 = declined
                }
                item.isPending = false;
                item.isDeclined = true;
                item.statusText = L"Declined";
                fName = item.fileName;
                sName = item.senderName;
                break;
            }
        }
    }
    if (!fName.empty()) {
        ShowActionToast(L"Declined file from " + sName, L"\xE711", RGB(239, 68, 68));
    }
    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

void RemoveNotification(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        g_notifications.erase(
            std::remove_if(g_notifications.begin(), g_notifications.end(), [&id](const NotificationItem& item) {
                return item.id == id;
            }),
            g_notifications.end()
        );
    }
    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

void MarkNotificationsCompleted(const std::string& remoteIp) {
    {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        for (auto& item : g_notifications) {
            if (item.isAccepted && !item.isCompleted) {
                if (remoteIp.empty() || item.senderIp == remoteIp) {
                    item.isCompleted = true;
                    item.statusText = L"Received successfully";
                }
            }
        }
    }
    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

void ClearResolvedNotifications() {
    {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        g_notifications.erase(
            std::remove_if(g_notifications.begin(), g_notifications.end(), [](const NotificationItem& item) {
                return !item.isPending;
            }),
            g_notifications.end()
        );
    }
    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

// UI Interaction state
struct NotifButtonHit {
    std::string id;
    bool isAccept;
    RECT rect;
};

struct CardCloseHit {
    std::string id;
    RECT rect;
};

static std::vector<NotifButtonHit> s_btnHits;
static std::vector<CardCloseHit> s_cardCloseHits;
static RECT s_clearAllRect = {};
static bool s_clearAllHovered = false;
static std::string s_hoveredBtnId = "";
static bool s_hoveredBtnIsAccept = false;
static std::string s_hoveredCardCloseId = "";

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
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);

    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

static std::wstring FormatSize(ULONGLONG bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    if (bytes < 1024 * 1024) return std::to_wstring(bytes / 1024) + L" KB";
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f MB", mb);
    return buf;
}

static std::wstring FormatTimeAgo(ULONGLONG timestamp) {
    ULONGLONG now = GetTickCount64();
    if (now < timestamp) return L"Just now";
    ULONGLONG diffSec = (now - timestamp) / 1000;
    if (diffSec < 10) return L"Just now";
    if (diffSec < 60) return std::to_wstring(diffSec) + L"s ago";
    ULONGLONG diffMin = diffSec / 60;
    if (diffMin < 60) return std::to_wstring(diffMin) + L"m ago";
    return std::to_wstring(diffMin / 60) + L"h ago";
}

int GetNotificationsTabDesiredWidth(int dockEdge) {
    bool isVertical = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);
    return isVertical ? 280 : 460;
}

int GetNotificationsTabDesiredHeight(int dockEdge) {
    bool isVertical = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);
    return isVertical ? 460 : 280;
}

void RenderNotificationsTab(HDC hdc, int width, int height, bool isDark, int dockEdge) {
    CheckAndExpireNotifications();
    bool isVertical = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);
    bool isLeft = (dockEdge == EDGE_LEFT);

    s_btnHits.clear();
    s_cardCloseHits.clear();
    s_clearAllRect = {0, 0, 0, 0};

    int contentLeft, contentTop, contentRight, contentBottom;
    if (isVertical) {
        contentLeft = isLeft ? 42 : 12;
        contentRight = isLeft ? (width - 12) : (width - 42);
        contentTop = 14;
        contentBottom = height - 14;
    } else {
        contentLeft = 14;
        contentRight = width - 14;
        contentTop = 36;
        contentBottom = height - 10;
    }

    if (contentRight <= contentLeft + 40 || contentBottom <= contentTop + 40) return;

    SetBkMode(hdc, TRANSPARENT);

    HFONT titleFont = CreateClearTypeFont(14, FW_BOLD);
    HFONT boldFont = CreateClearTypeFont(12, FW_SEMIBOLD);
    HFONT secFont = CreateClearTypeFont(11, FW_NORMAL);
    HFONT iconFont = CreateClearTypeFont(15, FW_NORMAL, L"Segoe Fluent Icons");
    HFONT smallIconFont = CreateClearTypeFont(12, FW_NORMAL, L"Segoe Fluent Icons");

    // Header: Title & Clear All
    int headerH = 26;
    RECT titleRc = {contentLeft + 4, contentTop, contentLeft + 180, contentTop + headerH};
    SelectObject(hdc, titleFont);
    SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
    DrawTextW(hdc, L"Notifications", -1, &titleRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    // Pending count pill next to title
    int pendingCount = GetPendingNotificationCount();
    if (pendingCount > 0) {
        RECT badgeRc = {contentLeft + 104, contentTop + 4, contentLeft + 128, contentTop + 22};
        DrawRoundedRect(hdc, badgeRc, 9, RGB(239, 68, 68), RGB(239, 68, 68), 0);
        SelectObject(hdc, boldFont);
        SetTextColor(hdc, RGB(255, 255, 255));
        std::wstring pStr = std::to_wstring(pendingCount);
        DrawTextW(hdc, pStr.c_str(), -1, &badgeRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }

    // Clear All button
    bool hasResolved = false;
    std::vector<NotificationItem> currentList;
    {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        currentList = g_notifications;
        for (const auto& it : currentList) {
            if (!it.isPending) { hasResolved = true; break; }
        }
    }

    if (hasResolved) {
        int clearW = 64;
        s_clearAllRect = {contentRight - clearW - 4, contentTop + 2, contentRight - 4, contentTop + 22};
        COLORREF clearBg = s_clearAllHovered ? (isDark ? RGB(45, 52, 70) : RGB(220, 228, 240))
                                             : (isDark ? RGB(28, 33, 44) : RGB(235, 240, 248));
        COLORREF clearBorder = s_clearAllHovered ? (isDark ? RGB(75, 88, 115) : RGB(180, 195, 215))
                                                 : (isDark ? RGB(45, 52, 70) : RGB(210, 220, 235));
        DrawRoundedRect(hdc, s_clearAllRect, 6, clearBg, clearBorder, 1);
        SelectObject(hdc, secFont);
        SetTextColor(hdc, isDark ? RGB(160, 172, 195) : RGB(90, 100, 120));
        DrawTextW(hdc, L"Clear All", -1, &s_clearAllRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }

    // Divider line below header
    int divY = contentTop + headerH + 4;
    HPEN divPen = CreatePen(PS_SOLID, 1, isDark ? RGB(45, 52, 68) : RGB(215, 222, 232));
    HGDIOBJ oldP = SelectObject(hdc, divPen);
    MoveToEx(hdc, contentLeft + 4, divY, nullptr);
    LineTo(hdc, contentRight - 4, divY);
    SelectObject(hdc, oldP);
    DeleteObject(divPen);

    // List container
    int listTop = divY + 8;
    int listBottom = contentBottom;

    if (currentList.empty()) {
        // Empty State
        int centerY = listTop + (listBottom - listTop) / 2 - 10;
        RECT iconRc = {contentLeft, centerY - 24, contentRight, centerY + 8};
        SelectObject(hdc, iconFont);
        SetTextColor(hdc, isDark ? RGB(90, 102, 125) : RGB(80, 92, 112));
        DrawTextW(hdc, L"\xE7E7", -1, &iconRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        RECT emptyTitleRc = {contentLeft, centerY + 12, contentRight, centerY + 28};
        SelectObject(hdc, boldFont);
        SetTextColor(hdc, isDark ? RGB(180, 192, 215) : RGB(40, 50, 68));
        DrawTextW(hdc, L"No new notifications", -1, &emptyTitleRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        RECT emptySubRc = {contentLeft, centerY + 30, contentRight, centerY + 46};
        SelectObject(hdc, secFont);
        SetTextColor(hdc, isDark ? RGB(110, 122, 142) : RGB(65, 78, 98));
        DrawTextW(hdc, L"Incoming AirDrop share requests will appear here.", -1, &emptySubRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    } else {
        int cardY = listTop;
        int cardH = 100;
        int cardGap = 8;
        int maxCards = (listBottom - listTop) / (cardH + cardGap);
        if (maxCards < 1) maxCards = 1;

        for (size_t i = 0; i < currentList.size() && static_cast<int>(i) < maxCards; ++i) {
            const auto& item = currentList[i];
            RECT cardRc = {contentLeft + 4, cardY, contentRight - 4, cardY + cardH};

            COLORREF cardBg = isDark ? RGB(24, 30, 42) : RGB(240, 245, 252);
            COLORREF cardBorder = item.isPending ? (isDark ? RGB(0, 120, 215) : RGB(0, 140, 240))
                                                 : (isDark ? RGB(45, 52, 68) : RGB(215, 222, 232));
            DrawRoundedRect(hdc, cardRc, 10, cardBg, cardBorder, item.isPending ? 2 : 1);

            // Left icon badge (AirDrop transfer icon)
            RECT iconBadgeRc = {cardRc.left + 10, cardRc.top + 10, cardRc.left + 42, cardRc.top + 42};
            COLORREF badgeBg = isDark ? RGB(16, 42, 75) : RGB(215, 235, 255);
            DrawRoundedRect(hdc, iconBadgeRc, 8, badgeBg, RGB(0, 160, 255), 1);
            SelectObject(hdc, iconFont);
            SetTextColor(hdc, RGB(0, 160, 255));
            DrawTextW(hdc, L"\xE701", -1, &iconBadgeRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            // Sender & Time
            int textLeft = iconBadgeRc.right + 10;
            RECT senderRc = {textLeft, cardRc.top + 10, cardRc.right - 95, cardRc.top + 26};
            SelectObject(hdc, boldFont);
            SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(18, 24, 34));
            std::wstring senderDisplay = L"AirDrop: " + item.senderName;
            DrawTextW(hdc, senderDisplay.c_str(), -1, &senderRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

            // Time Ago
            RECT timeRc = {cardRc.right - 92, cardRc.top + 10, cardRc.right - 28, cardRc.top + 26};
            SelectObject(hdc, secFont);
            SetTextColor(hdc, isDark ? RGB(120, 132, 155) : RGB(65, 78, 98));
            std::wstring timeAgo = FormatTimeAgo(item.timestamp);
            DrawTextW(hdc, timeAgo.c_str(), -1, &timeRc, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

            // Individual Card Close [X] Button
            RECT closeRc = {cardRc.right - 24, cardRc.top + 8, cardRc.right - 6, cardRc.top + 26};
            bool isCloseHov = (s_hoveredCardCloseId == item.id);
            if (isCloseHov) {
                COLORREF cBg = isDark ? RGB(55, 24, 28) : RGB(255, 225, 230);
                DrawRoundedRect(hdc, closeRc, 4, cBg, RGB(239, 68, 68), 1);
            }
            SelectObject(hdc, smallIconFont);
            SetTextColor(hdc, isCloseHov ? RGB(239, 68, 68) : (isDark ? RGB(130, 140, 160) : RGB(70, 80, 100)));
            DrawTextW(hdc, L"\xE711", -1, &closeRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            s_cardCloseHits.push_back({item.id, closeRc});

            // File details box
            RECT fileBoxRc = {textLeft, cardRc.top + 28, cardRc.right - 10, cardRc.top + 54};
            COLORREF fbBg = isDark ? RGB(18, 22, 32) : RGB(226, 234, 246);
            DrawRoundedRect(hdc, fileBoxRc, 6, fbBg, isDark ? RGB(38, 45, 60) : RGB(205, 215, 230), 1);

            RECT fileIconRc = {fileBoxRc.left + 6, fileBoxRc.top, fileBoxRc.left + 22, fileBoxRc.bottom};
            SelectObject(hdc, smallIconFont);
            SetTextColor(hdc, isDark ? RGB(160, 175, 205) : RGB(50, 65, 90));
            DrawTextW(hdc, L"\xE8A5", -1, &fileIconRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            RECT fileNameRc = {fileBoxRc.left + 26, fileBoxRc.top, fileBoxRc.right - 70, fileBoxRc.bottom};
            SelectObject(hdc, boldFont);
            SetTextColor(hdc, isDark ? RGB(240, 245, 255) : RGB(20, 28, 42));
            DrawTextW(hdc, item.fileName.c_str(), -1, &fileNameRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

            RECT fileSizeRc = {fileBoxRc.right - 65, fileBoxRc.top, fileBoxRc.right - 6, fileBoxRc.bottom};
            SelectObject(hdc, secFont);
            SetTextColor(hdc, isDark ? RGB(140, 152, 175) : RGB(60, 70, 88));
            std::wstring sizeStr = FormatSize(item.fileSize);
            DrawTextW(hdc, sizeStr.c_str(), -1, &fileSizeRc, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

            // Action Buttons / Status
            if (item.isPending) {
                ULONGLONG nowTick = GetTickCount64();
                int remSec = (item.expiresAt > nowTick) ? static_cast<int>((item.expiresAt - nowTick) / 1000) : 0;

                int btnW = 76;
                int btnH = 26;
                int btnY = cardRc.bottom - btnH - 8;

                // Expiry countdown pill on the left
                RECT expiryRc = {textLeft, btnY, cardRc.right - btnW * 2 - 20, btnY + btnH};
                SelectObject(hdc, secFont);
                SetTextColor(hdc, (remSec <= 15) ? RGB(239, 68, 68) : (isDark ? RGB(245, 158, 11) : RGB(180, 100, 0)));
                std::wstring expText = L"Expires: " + std::to_wstring(remSec) + L"s";
                DrawTextW(hdc, expText.c_str(), -1, &expiryRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

                // [Accept] Button
                RECT acceptRc = {cardRc.right - btnW * 2 - 12, btnY, cardRc.right - btnW - 6, btnY + btnH};
                bool isAcceptHover = (s_hoveredBtnId == item.id && s_hoveredBtnIsAccept);
                COLORREF accBg = isAcceptHover ? RGB(5, 150, 105) : RGB(16, 185, 129);
                DrawRoundedRect(hdc, acceptRc, 6, accBg, accBg, 0);

                SelectObject(hdc, boldFont);
                SetTextColor(hdc, RGB(255, 255, 255));
                DrawTextW(hdc, L"Accept", -1, &acceptRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                // [Decline] Button
                RECT declineRc = {cardRc.right - btnW, btnY, cardRc.right - 4, btnY + btnH};
                bool isDeclineHover = (s_hoveredBtnId == item.id && !s_hoveredBtnIsAccept);
                COLORREF decBg = isDeclineHover ? (isDark ? RGB(65, 25, 30) : RGB(255, 225, 225))
                                                : (isDark ? RGB(32, 38, 50) : RGB(230, 236, 246));
                COLORREF decBorder = isDeclineHover ? RGB(239, 68, 68)
                                                    : (isDark ? RGB(60, 70, 90) : RGB(195, 205, 220));
                DrawRoundedRect(hdc, declineRc, 6, decBg, decBorder, 1);

                SelectObject(hdc, secFont);
                SetTextColor(hdc, isDeclineHover ? RGB(239, 68, 68) : (isDark ? RGB(200, 210, 230) : RGB(40, 50, 68)));
                DrawTextW(hdc, L"Decline", -1, &declineRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                s_btnHits.push_back({item.id, true, acceptRc});
                s_btnHits.push_back({item.id, false, declineRc});
            } else {
                RECT statusRc = {textLeft, cardRc.bottom - 26, cardRc.right - 10, cardRc.bottom - 6};
                SelectObject(hdc, boldFont);
                if (item.isCompleted) {
                    SetTextColor(hdc, RGB(52, 199, 89));
                    DrawTextW(hdc, L"\xE73E  Received successfully", -1, &statusRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
                } else if (item.isAccepted) {
                    SetTextColor(hdc, RGB(0, 160, 255));
                    DrawTextW(hdc, L"\xE895  Receiving files...", -1, &statusRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
                } else if (item.isDeclined) {
                    SetTextColor(hdc, RGB(239, 68, 68));
                    DrawTextW(hdc, L"\xE711  Declined", -1, &statusRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
                } else if (item.isExpired) {
                    SetTextColor(hdc, RGB(239, 68, 68));
                    DrawTextW(hdc, L"\xE7BA  Expired (No response in 1 min)", -1, &statusRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
                } else {
                    SetTextColor(hdc, isDark ? RGB(140, 150, 170) : RGB(100, 110, 125));
                    DrawTextW(hdc, item.statusText.c_str(), -1, &statusRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
                }
            }

            cardY += cardH + cardGap;
        }
    }

    DeleteObject(titleFont);
    DeleteObject(boldFont);
    DeleteObject(secFont);
    DeleteObject(iconFont);
    DeleteObject(smallIconFont);
}

bool HandleNotificationsMouseMove(HWND hwnd, POINT pt) {
    bool needRepaint = false;

    bool newClearHover = PtInRect(&s_clearAllRect, pt);
    if (newClearHover != s_clearAllHovered) {
        s_clearAllHovered = newClearHover;
        needRepaint = true;
    }

    std::string newCloseHoverId = "";
    for (const auto& hit : s_cardCloseHits) {
        if (PtInRect(&hit.rect, pt)) {
            newCloseHoverId = hit.id;
            break;
        }
    }
    if (newCloseHoverId != s_hoveredCardCloseId) {
        s_hoveredCardCloseId = newCloseHoverId;
        needRepaint = true;
    }

    std::string newHoverId = "";
    bool newHoverIsAccept = false;
    for (const auto& hit : s_btnHits) {
        if (PtInRect(&hit.rect, pt)) {
            newHoverId = hit.id;
            newHoverIsAccept = hit.isAccept;
            break;
        }
    }

    if (newHoverId != s_hoveredBtnId || newHoverIsAccept != s_hoveredBtnIsAccept) {
        s_hoveredBtnId = newHoverId;
        s_hoveredBtnIsAccept = newHoverIsAccept;
        needRepaint = true;
    }

    if (needRepaint && hwnd && IsWindow(hwnd)) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return !newHoverId.empty() || !newCloseHoverId.empty() || newClearHover;
}

bool HandleNotificationsClick(HWND hwnd, POINT pt) {
    // 1. Clear individual notification
    for (const auto& hit : s_cardCloseHits) {
        if (PtInRect(&hit.rect, pt)) {
            RemoveNotification(hit.id);
            return true;
        }
    }

    // 2. Clear all resolved
    if (PtInRect(&s_clearAllRect, pt)) {
        ClearResolvedNotifications();
        return true;
    }

    // 3. Accept / Decline buttons
    for (const auto& hit : s_btnHits) {
        if (PtInRect(&hit.rect, pt)) {
            if (hit.isAccept) {
                AcceptNotification(hit.id);
            } else {
                DeclineNotification(hit.id);
            }
            return true;
        }
    }
    return false;
}

void HandleNotificationsMouseUp(HWND hwnd) {
}
