#include "share_ui.h"
#include "state.h"
#include "network.h"
#include "blur.h"
#include <qrcodegen.hpp>
#include <commdlg.h>
#include <cmath>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

static HWND s_shareHwnd = nullptr;
static std::wstring s_activeFilePath = L"";
static std::wstring s_statusMessage = L"Ready to share";
static bool s_isTransferring = false;

// Radar animation
static float s_sonarRadius = 0.0f;
static int s_hoveredPeerIndex = -1;
static bool s_browseHovered = false;
static bool s_copyLinkHovered = false;

// Hit test rectangles
static RECT s_browseRect = {};
static RECT s_copyLinkRect = {};
static std::vector<RECT> s_peerNodeRects;

// QR Code generation cache
static std::string s_cachedQrUrl = "";
static std::vector<std::vector<bool>> s_cachedQrModules;
static int s_cachedQrGrid = 0;

static void EnsureQrCached(const std::string& url) {
    if (url == s_cachedQrUrl && s_cachedQrGrid > 0) return;
    try {
        qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        s_cachedQrGrid = qr.getSize();
        s_cachedQrModules.assign(s_cachedQrGrid, std::vector<bool>(s_cachedQrGrid, false));
        for (int y = 0; y < s_cachedQrGrid; ++y) {
            for (int x = 0; x < s_cachedQrGrid; ++x) {
                s_cachedQrModules[y][x] = qr.getModule(x, y);
            }
        }
        s_cachedQrUrl = url;
    } catch (...) {
        s_cachedQrGrid = 0;
    }
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

static std::wstring GetFileNameOnly(const std::wstring& path)
{
    size_t last = path.find_last_of(L"\\/");
    if (last != std::wstring::npos) return path.substr(last + 1);
    return path;
}

void SetShareActiveFile(const std::wstring& path)
{
    s_activeFilePath = path;
    if (path.empty())
        s_statusMessage = L"Ready to share";
    else
        s_statusMessage = L"File ready: " + GetFileNameOnly(path);
}

int GetShareTabDesiredWidth(int dockEdge)
{
    if (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT)
        return 280;
    return 590;
}

int GetShareTabDesiredHeight(int dockEdge)
{
    if (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT)
        return 580;
    return 306;
}

void RenderShareTab(HDC hdc, int width, int height, bool isDark, int dockEdge)
{
    bool isVert = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);

    int savedDC = SaveDC(hdc);
    HRGN clipRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, 16, 16);
    SelectClipRgn(hdc, clipRgn);
    DeleteObject(clipRgn);

    if (!isVert && (width < 340 || height < 120)) {
        RestoreDC(hdc, savedDC);
        return;
    }
    if (isVert && (width < 100 || height < 180)) {
        RestoreDC(hdc, savedDC);
        return;
    }

    HFONT iconFont = CreateClearTypeFont(15, FW_NORMAL, L"Segoe MDL2 Assets");
    HFONT titleFont = CreateClearTypeFont(13, FW_BOLD, L"Segoe UI");
    HFONT secFont = CreateClearTypeFont(11, FW_NORMAL, L"Segoe UI");
    HFONT boldSecFont = CreateClearTypeFont(11, FW_BOLD, L"Segoe UI");
    HGDIOBJ oldF = SelectObject(hdc, titleFont);
    SetBkMode(hdc, TRANSPARENT);

    std::string localIp = NetworkManager::Instance().GetLocalIP();
    int httpPort = NetworkManager::Instance().GetHttpPort();
    std::string qrUrl = "http://" + localIp + ":" + std::to_string(httpPort) + "/";
    std::wstring wUrl(qrUrl.begin(), qrUrl.end());

    EnsureQrCached(qrUrl);

    if (!isVert)
    {
        // ==========================================
        // HORIZONTAL DOCK SHARE TAB (EDGE_TOP)
        // ==========================================
        int contentTop = 38;
        int contentBottom = height - 12;

        // --- LEFT PANE: Mobile Connect (QR Code) ---
        int leftCardW = 236;
        RECT leftCardRc = {14, contentTop, 14 + leftCardW, contentBottom};

        SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
        RECT qrTitleRc = {leftCardRc.left + 4, leftCardRc.top + 2, leftCardRc.right - 4, leftCardRc.top + 20};
        DrawTextW(hdc, L"Mobile Connect", -1, &qrTitleRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        SelectObject(hdc, secFont);
        SetTextColor(hdc, isDark ? RGB(150, 160, 180) : RGB(100, 110, 125));
        RECT qrSubRc = {leftCardRc.left + 4, leftCardRc.top + 20, leftCardRc.right - 4, leftCardRc.top + 34};
        DrawTextW(hdc, L"Scan with phone camera to share", -1, &qrSubRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        // Enlarged QR Code Card (152px)
        int qrCardSize = 152;
        int qrCardX = leftCardRc.left + (leftCardW - qrCardSize) / 2;
        int qrCardY = leftCardRc.top + 38;
        RECT qrCardRc = {qrCardX, qrCardY, qrCardX + qrCardSize, qrCardY + qrCardSize};
        DrawRoundedRect(hdc, qrCardRc, 8, RGB(255, 255, 255), isDark ? RGB(70, 80, 100) : RGB(190, 200, 215), 1);

        if (s_cachedQrGrid > 0) {
            int qrGrid = s_cachedQrGrid;
            int margin = 8;
            int usableSize = qrCardSize - margin * 2;
            int modulePixel = usableSize / qrGrid;
            if (modulePixel < 1) modulePixel = 1;
            int offsetLeft = qrCardX + (qrCardSize - modulePixel * qrGrid) / 2;
            int offsetTop = qrCardY + (qrCardSize - modulePixel * qrGrid) / 2;

            HBRUSH blackBr = CreateSolidBrush(RGB(0, 0, 0));
            for (int y = 0; y < qrGrid; y++) {
                for (int x = 0; x < qrGrid; x++) {
                    if (s_cachedQrModules[y][x]) {
                        RECT mRc = {
                            offsetLeft + x * modulePixel,
                            offsetTop + y * modulePixel,
                            offsetLeft + (x + 1) * modulePixel,
                            offsetTop + (y + 1) * modulePixel
                        };
                        FillRect(hdc, &mRc, blackBr);
                    }
                }
            }
            DeleteObject(blackBr);
        }

        // Clickable URL pill
        int linkY = qrCardY + qrCardSize + 6;
        s_copyLinkRect = {leftCardRc.left + 8, linkY, leftCardRc.right - 8, linkY + 22};
        COLORREF linkBg = s_copyLinkHovered ? (isDark ? RGB(20, 50, 95) : RGB(215, 235, 255))
                                            : (isDark ? RGB(24, 30, 42) : RGB(230, 236, 246));
        COLORREF linkBorder = s_copyLinkHovered ? RGB(0, 150, 245) : (isDark ? RGB(50, 60, 80) : RGB(200, 210, 226));
        DrawRoundedRect(hdc, s_copyLinkRect, 6, linkBg, linkBorder, 1);

        SelectObject(hdc, boldSecFont);
        SetTextColor(hdc, s_copyLinkHovered ? RGB(0, 160, 255) : (isDark ? RGB(100, 180, 255) : RGB(0, 110, 210)));
        DrawTextW(hdc, wUrl.c_str(), -1, &s_copyLinkRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // Network hint note
        RECT hintRc = {leftCardRc.left + 2, linkY + 23, leftCardRc.right - 2, linkY + 36};
        SelectObject(hdc, secFont);
        SetTextColor(hdc, isDark ? RGB(110, 122, 145) : RGB(140, 150, 168));
        DrawTextW(hdc, L"Same Wi-Fi (Set PC to Private Network)", -1, &hintRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // --- VERTICAL DIVIDER ---
        int divX = leftCardRc.right + 10;
        COLORREF divCol = isDark ? RGB(45, 52, 68) : RGB(210, 218, 230);
        HPEN divPen = CreatePen(PS_SOLID, 1, divCol);
        HGDIOBJ oldP = SelectObject(hdc, divPen);
        MoveToEx(hdc, divX, contentTop + 8, nullptr);
        LineTo(hdc, divX, contentBottom - 8);
        SelectObject(hdc, oldP);
        DeleteObject(divPen);

        // --- RIGHT PANE: Sonar Radar & Discovered PCs ---
        int rightLeft = divX + 12;
        int rightWidth = width - rightLeft - 14;

        if (rightWidth > 60 && contentBottom > contentTop + 80)
        {
            SelectObject(hdc, titleFont);
            SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
            RECT radarTitleRc = {rightLeft + 4, contentTop + 2, rightLeft + 160, contentTop + 20};
            DrawTextW(hdc, L"Local Wi-Fi Radar", -1, &radarTitleRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            // Radar center
            int radarCenterX = rightLeft + rightWidth / 2;
            int radarCenterY = contentTop + 95;

            // 3 Concentric circles
            COLORREF ringCol = isDark ? RGB(32, 44, 62) : RGB(210, 222, 238);
            HPEN ringPen = CreatePen(PS_SOLID, 1, ringCol);
            oldP = SelectObject(hdc, ringPen);
            HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));

            for (int r : {22, 44, 66}) {
                Ellipse(hdc, radarCenterX - r, radarCenterY - r, radarCenterX + r, radarCenterY + r);
            }

            // Animated continuous sonar wave ripples based on wall-clock time
            ULONGLONG nowTick = GetTickCount64();
            for (int offset : {0, 1100}) {
                float pNorm = static_cast<float>((nowTick + offset) % 2200) / 2200.0f;
                int pulseR = static_cast<int>(6.0f + pNorm * 62.0f);
                if (pulseR > 6 && pulseR < 70) {
                    COLORREF pulseCol = isDark ? (pNorm < 0.6f ? RGB(0, 150, 245) : RGB(0, 100, 180))
                                               : (pNorm < 0.6f ? RGB(30, 140, 235) : RGB(100, 180, 245));
                    HPEN pulsePen = CreatePen(PS_SOLID, pNorm < 0.5f ? 2 : 1, pulseCol);
                    HGDIOBJ pOld = SelectObject(hdc, pulsePen);
                    Ellipse(hdc, radarCenterX - pulseR, radarCenterY - pulseR,
                            radarCenterX + pulseR, radarCenterY + pulseR);
                    SelectObject(hdc, pOld);
                    DeleteObject(pulsePen);
                }
            }

            SelectObject(hdc, oldB);
            SelectObject(hdc, oldP);
            DeleteObject(ringPen);

            // Center Node: You
            RECT centerRc = {radarCenterX - 14, radarCenterY - 14, radarCenterX + 14, radarCenterY + 14};
            DrawRoundedRect(hdc, centerRc, 14, RGB(0, 120, 215), RGB(0, 160, 255), 2);
            SelectObject(hdc, iconFont);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, L"\xE7F8", -1, &centerRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            RECT youRc = {radarCenterX - 50, radarCenterY + 16, radarCenterX + 50, radarCenterY + 30};
            SelectObject(hdc, secFont);
            SetTextColor(hdc, isDark ? RGB(180, 195, 220) : RGB(70, 80, 100));
            std::wstring myName = g_settings.username;
            if (myName.empty()) myName = L"You";
            DrawTextW(hdc, (myName + L" (You)").c_str(), -1, &youRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            // Discovered Peer Nodes
            s_peerNodeRects.clear();
            std::vector<DiscoveredPeer> currentPeers;
            {
                std::lock_guard<std::mutex> lock(g_peersMutex);
                currentPeers = g_peers;
            }

            if (currentPeers.empty()) {
                SelectObject(hdc, secFont);
                SetTextColor(hdc, isDark ? RGB(110, 122, 142) : RGB(140, 150, 165));
                RECT searchRc = {rightLeft, radarCenterY + 44, rightLeft + rightWidth, radarCenterY + 60};
                DrawTextW(hdc, L"Scanning local Wi-Fi for nearby WinTray PCs...", -1, &searchRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            } else {
                for (size_t i = 0; i < currentPeers.size(); ++i) {
                    const auto& peer = currentPeers[i];
                    unsigned int h = 0;
                    for (char c : peer.ip) h = h * 31 + static_cast<unsigned char>(c);
                    float angle = static_cast<float>(h % 360) * (3.14159265f / 180.0f);
                    float dist = 40.0f + static_cast<float>(h % 22);

                    int px = radarCenterX + static_cast<int>(dist * cos(angle));
                    int py = radarCenterY + static_cast<int>(dist * sin(angle));

                    RECT nodeRc = {px - 14, py - 14, px + 14, py + 14};
                    s_peerNodeRects.push_back(nodeRc);

                    bool isHovered = (s_hoveredPeerIndex == static_cast<int>(i));
                    COLORREF nBg = isHovered ? (isDark ? RGB(20, 65, 125) : RGB(180, 215, 255))
                                             : (isDark ? RGB(28, 35, 48) : RGB(225, 235, 248));
                    COLORREF nBorder = isHovered ? RGB(0, 160, 255)
                                                 : (isDark ? RGB(65, 80, 105) : RGB(185, 198, 220));

                    DrawRoundedRect(hdc, nodeRc, 14, nBg, nBorder, isHovered ? 2 : 1);
                    SelectObject(hdc, iconFont);
                    SetTextColor(hdc, isHovered ? RGB(0, 160, 255) : (isDark ? RGB(200, 210, 230) : RGB(60, 70, 90)));
                    DrawTextW(hdc, L"\xE7F8", -1, &nodeRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                    RECT pNameRc = {px - 45, py + 16, px + 45, py + 28};
                    SelectObject(hdc, boldSecFont);
                    SetTextColor(hdc, isDark ? RGB(235, 240, 252) : RGB(20, 30, 45));
                    DrawTextW(hdc, peer.name.c_str(), -1, &pNameRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                }
            }

            // Bottom File Bar on Right Side
            int fileBarY = contentBottom - 34;
            RECT fileCardRc = {rightLeft + 4, fileBarY, rightLeft + rightWidth - 85, fileBarY + 30};
            COLORREF fcBg = isDark ? RGB(22, 26, 36) : RGB(230, 236, 246);
            COLORREF fcBorder = isDark ? RGB(50, 58, 75) : RGB(200, 210, 225);
            DrawRoundedRect(hdc, fileCardRc, 8, fcBg, fcBorder, 1);

            SelectObject(hdc, secFont);
            SetTextColor(hdc, isDark ? RGB(215, 225, 245) : RGB(30, 40, 55));
            std::wstring fileLabel;
            if (!s_statusMessage.empty() && s_statusMessage != L"Ready to share") {
                fileLabel = s_statusMessage;
            } else if (!s_activeFilePath.empty()) {
                fileLabel = L"Send: " + GetFileNameOnly(s_activeFilePath);
            } else {
                fileLabel = L"Drop file or Browse ->";
            }
            RECT fTextRc = {fileCardRc.left + 8, fileCardRc.top, fileCardRc.right - 8, fileCardRc.bottom};
            DrawTextW(hdc, fileLabel.c_str(), -1, &fTextRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

            // Browse Button
            s_browseRect = {rightLeft + rightWidth - 80, fileBarY, rightLeft + rightWidth, fileBarY + 30};
            COLORREF bBg = s_browseHovered ? (isDark ? RGB(32, 90, 170) : RGB(190, 220, 255))
                                           : (isDark ? RGB(24, 75, 140) : RGB(210, 230, 255));
            DrawRoundedRect(hdc, s_browseRect, 8, bBg, RGB(0, 120, 215), 1);
            SelectObject(hdc, boldSecFont);
            SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(0, 90, 180));
            DrawTextW(hdc, L"Browse...", -1, &s_browseRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        else
        {
            s_peerNodeRects.clear();
            s_browseRect = {0, 0, 0, 0};
        }
    }
    else
    {
        // ==========================================
        // VERTICAL DOCK SHARE TAB (EDGE_LEFT / RIGHT)
        // ==========================================
        int contentLeft = (dockEdge == EDGE_LEFT) ? 40 : 12;
        int contentW = width - 52;
        int topY = 24;

        SelectObject(hdc, titleFont);
        SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
        RECT qrTitleRc = {contentLeft, topY, contentLeft + contentW, topY + 20};
        DrawTextW(hdc, L"AirDrop / Share", -1, &qrTitleRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // Enlarged QR Code (160px)
        int qrCardSize = 160;
        int qrCardX = contentLeft + (contentW - qrCardSize) / 2;
        int qrCardY = topY + 24;
        RECT qrCardRc = {qrCardX, qrCardY, qrCardX + qrCardSize, qrCardY + qrCardSize};
        DrawRoundedRect(hdc, qrCardRc, 8, RGB(255, 255, 255), isDark ? RGB(70, 80, 100) : RGB(190, 200, 215), 1);

        if (s_cachedQrGrid > 0) {
            int qrGrid = s_cachedQrGrid;
            int margin = 8;
            int usableSize = qrCardSize - margin * 2;
            int modulePixel = usableSize / qrGrid;
            if (modulePixel < 1) modulePixel = 1;
            int offsetLeft = qrCardX + (qrCardSize - modulePixel * qrGrid) / 2;
            int offsetTop = qrCardY + (qrCardSize - modulePixel * qrGrid) / 2;

            HBRUSH blackBr = CreateSolidBrush(RGB(0, 0, 0));
            for (int y = 0; y < qrGrid; y++) {
                for (int x = 0; x < qrGrid; x++) {
                    if (s_cachedQrModules[y][x]) {
                        RECT mRc = {
                            offsetLeft + x * modulePixel,
                            offsetTop + y * modulePixel,
                            offsetLeft + (x + 1) * modulePixel,
                            offsetTop + (y + 1) * modulePixel
                        };
                        FillRect(hdc, &mRc, blackBr);
                    }
                }
            }
            DeleteObject(blackBr);
        }

        int linkY = qrCardY + qrCardSize + 6;
        s_copyLinkRect = {contentLeft + 8, linkY, contentLeft + contentW - 8, linkY + 22};
        COLORREF linkBg = s_copyLinkHovered ? (isDark ? RGB(20, 50, 95) : RGB(215, 235, 255))
                                            : (isDark ? RGB(24, 30, 42) : RGB(230, 236, 246));
        DrawRoundedRect(hdc, s_copyLinkRect, 6, linkBg, RGB(0, 120, 215), 1);
        SelectObject(hdc, boldSecFont);
        SetTextColor(hdc, s_copyLinkHovered ? RGB(0, 160, 255) : (isDark ? RGB(100, 180, 255) : RGB(0, 110, 210)));
        DrawTextW(hdc, wUrl.c_str(), -1, &s_copyLinkRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // Divider
        int divY = linkY + 30;
        COLORREF divCol = isDark ? RGB(45, 52, 68) : RGB(210, 218, 230);
        HPEN divPen = CreatePen(PS_SOLID, 1, divCol);
        HGDIOBJ oldP = SelectObject(hdc, divPen);
        MoveToEx(hdc, contentLeft + 10, divY, nullptr);
        LineTo(hdc, contentLeft + contentW - 10, divY);
        SelectObject(hdc, oldP);
        DeleteObject(divPen);

        if (height > 320)
        {
            // Radar
            int radarCenterX = contentLeft + contentW / 2;
            int radarCenterY = divY + 80;

            COLORREF ringCol = isDark ? RGB(32, 44, 62) : RGB(210, 222, 238);
            HPEN ringPen = CreatePen(PS_SOLID, 1, ringCol);
            oldP = SelectObject(hdc, ringPen);
            HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));

            for (int r : {22, 44, 66}) {
                Ellipse(hdc, radarCenterX - r, radarCenterY - r, radarCenterX + r, radarCenterY + r);
            }

            ULONGLONG nowTick = GetTickCount64();
            for (int offset : {0, 1100}) {
                float pNorm = static_cast<float>((nowTick + offset) % 2200) / 2200.0f;
                int pulseR = static_cast<int>(6.0f + pNorm * 62.0f);
                if (pulseR > 6 && pulseR < 70) {
                    COLORREF pulseCol = isDark ? (pNorm < 0.6f ? RGB(0, 150, 245) : RGB(0, 100, 180))
                                               : (pNorm < 0.6f ? RGB(30, 140, 235) : RGB(100, 180, 245));
                    HPEN pulsePen = CreatePen(PS_SOLID, pNorm < 0.5f ? 2 : 1, pulseCol);
                    HGDIOBJ pOld = SelectObject(hdc, pulsePen);
                    Ellipse(hdc, radarCenterX - pulseR, radarCenterY - pulseR,
                            radarCenterX + pulseR, radarCenterY + pulseR);
                    SelectObject(hdc, pOld);
                    DeleteObject(pulsePen);
                }
            }
            SelectObject(hdc, oldB);
            SelectObject(hdc, oldP);
            DeleteObject(ringPen);

            RECT centerRc = {radarCenterX - 14, radarCenterY - 14, radarCenterX + 14, radarCenterY + 14};
            DrawRoundedRect(hdc, centerRc, 14, RGB(0, 120, 215), RGB(0, 160, 255), 2);
            SelectObject(hdc, iconFont);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, L"\xE7F8", -1, &centerRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            // Discovered Peer Nodes in vertical mode
            s_peerNodeRects.clear();
            std::vector<DiscoveredPeer> currentPeers;
            {
                std::lock_guard<std::mutex> lock(g_peersMutex);
                currentPeers = g_peers;
            }

            for (size_t i = 0; i < currentPeers.size(); ++i) {
                const auto& peer = currentPeers[i];
                unsigned int h = 0;
                for (char c : peer.ip) h = h * 31 + static_cast<unsigned char>(c);
                float angle = static_cast<float>(h % 360) * (3.14159265f / 180.0f);
                float dist = 38.0f + static_cast<float>(h % 22);

                int px = radarCenterX + static_cast<int>(dist * cos(angle));
                int py = radarCenterY + static_cast<int>(dist * sin(angle));

                RECT nodeRc = {px - 14, py - 14, px + 14, py + 14};
                s_peerNodeRects.push_back(nodeRc);

                bool isHovered = (s_hoveredPeerIndex == static_cast<int>(i));
                COLORREF nBg = isHovered ? (isDark ? RGB(20, 65, 125) : RGB(180, 215, 255))
                                         : (isDark ? RGB(28, 35, 48) : RGB(225, 235, 248));
                DrawRoundedRect(hdc, nodeRc, 14, nBg, isHovered ? RGB(0, 160, 255) : RGB(70, 85, 110), 1);

                SelectObject(hdc, iconFont);
                SetTextColor(hdc, isHovered ? RGB(0, 160, 255) : RGB(200, 210, 230));
                DrawTextW(hdc, L"\xE7F8", -1, &nodeRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }

            // Bottom Browse Button
            int bBtnY = height - 44;
            s_browseRect = {contentLeft + 10, bBtnY, contentLeft + contentW - 10, bBtnY + 32};
            COLORREF bBg = s_browseHovered ? (isDark ? RGB(32, 90, 170) : RGB(190, 220, 255))
                                           : (isDark ? RGB(24, 75, 140) : RGB(210, 230, 255));
            DrawRoundedRect(hdc, s_browseRect, 8, bBg, RGB(0, 120, 215), 1);
            SelectObject(hdc, boldSecFont);
            SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(0, 90, 180));
            std::wstring bLabel = !s_activeFilePath.empty() ? (L"Send: " + GetFileNameOnly(s_activeFilePath)) : L"Browse File to Send...";
            if (!s_statusMessage.empty() && s_statusMessage != L"Ready to share") {
                bLabel = s_statusMessage;
            }
            DrawTextW(hdc, bLabel.c_str(), -1, &s_browseRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        }
        else
        {
            s_peerNodeRects.clear();
            s_browseRect = {0, 0, 0, 0};
        }
    }

    SelectObject(hdc, oldF);
    DeleteObject(iconFont);
    DeleteObject(titleFont);
    DeleteObject(secFont);
    DeleteObject(boldSecFont);
    RestoreDC(hdc, savedDC);
}

bool HandleShareTabMouseMove(HWND hwnd, POINT pt)
{
    bool needRepaint = false;

    bool browseHov = PtInRect(&s_browseRect, pt);
    if (browseHov != s_browseHovered) {
        s_browseHovered = browseHov;
        needRepaint = true;
    }

    bool linkHov = PtInRect(&s_copyLinkRect, pt);
    if (linkHov != s_copyLinkHovered) {
        s_copyLinkHovered = linkHov;
        needRepaint = true;
    }

    int peerHov = -1;
    for (size_t i = 0; i < s_peerNodeRects.size(); ++i) {
        if (PtInRect(&s_peerNodeRects[i], pt)) {
            peerHov = static_cast<int>(i);
            break;
        }
    }
    if (peerHov != s_hoveredPeerIndex) {
        s_hoveredPeerIndex = peerHov;
        needRepaint = true;
    }

    if (needRepaint) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return (browseHov || linkHov || peerHov != -1);
}

bool HandleShareTabClick(HWND hwnd, POINT pt)
{
    // Browse File button
    if (PtInRect(&s_browseRect, pt)) {
        wchar_t szFile[MAX_PATH] = {0};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

        if (GetOpenFileNameW(&ofn)) {
            s_activeFilePath = szFile;
            s_statusMessage = L"Ready to send: " + GetFileNameOnly(s_activeFilePath);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }

    // Click Copy Link
    if (PtInRect(&s_copyLinkRect, pt)) {
        std::string localIp = NetworkManager::Instance().GetLocalIP();
        int httpPort = NetworkManager::Instance().GetHttpPort();
        std::string qrUrl = "http://" + localIp + ":" + std::to_string(httpPort) + "/";
        std::wstring wUrl(qrUrl.begin(), qrUrl.end());

        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            size_t sz = (wUrl.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sz);
            if (hMem) {
                memcpy(GlobalLock(hMem), wUrl.c_str(), sz);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
            CloseClipboard();
            s_statusMessage = L"Link copied to clipboard!";
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }

    // Click on Discovered Peer to send file
    for (size_t i = 0; i < s_peerNodeRects.size(); ++i) {
        if (PtInRect(&s_peerNodeRects[i], pt)) {
            if (s_activeFilePath.empty()) {
                // Auto-open browse dialog if no file is selected
                wchar_t szFile[MAX_PATH] = {0};
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile);
                ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

                if (GetOpenFileNameW(&ofn)) {
                    s_activeFilePath = szFile;
                } else {
                    return true;
                }
            }

            if (s_isTransferring) return true;

            DiscoveredPeer targetPeer;
            {
                std::lock_guard<std::mutex> lock(g_peersMutex);
                if (i < g_peers.size()) targetPeer = g_peers[i];
                else return true;
            }

            s_isTransferring = true;
            s_statusMessage = L"Sending file to " + targetPeer.name + L"...";
            InvalidateRect(hwnd, nullptr, FALSE);

            std::wstring filePath = s_activeFilePath;
            std::thread([hwnd, targetPeer, filePath]() {
                std::string err;
                bool ok = NetworkManager::Instance().SendFileToPeer(targetPeer.ip, filePath, err, targetPeer.port);
                s_isTransferring = false;
                if (ok) {
                    s_statusMessage = L"Transfer to " + targetPeer.name + L" completed successfully!";
                } else {
                    std::wstring wErr(err.begin(), err.end());
                    s_statusMessage = L"Transfer failed: " + wErr;
                }
                if (IsWindow(hwnd)) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }).detach();

            return true;
        }
    }

    return false;
}

// Standalone dialog or dock switch
void OpenShareDialog(HWND parentHwnd, const std::wstring &initialFilePath)
{
    SetShareActiveFile(initialFilePath);

    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        g_activeTab = TAB_SHARE;
        TriggerExpand(g_mainHwnd);
        CalculateTargetSize(true);
        SetTimer(g_mainHwnd, ANIM_TIMER_ID, 12, nullptr);
        SetForegroundWindow(g_mainHwnd);
        InvalidateRect(g_mainHwnd, nullptr, TRUE);
    }
}
