#include "share_ui.h"
#include "state.h"
#include "network.h"
#include "settings.h"
#include "blur.h"
#include <qrcodegen.hpp>
#include <commdlg.h>
#include <cmath>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>

static inline int ClampInt(int val, int low, int high) {
    return (val < low) ? low : (val > high ? high : val);
}

static HWND s_shareHwnd = nullptr;
static std::vector<std::wstring> s_activeFilePaths;
static std::wstring s_statusMessage = L"Ready to share";
static bool s_isTransferring = false;

// Source picker popup state
static bool s_showSourcePicker = false;
static DiscoveredPeer s_chosenPeer = {};

// Expiration / Retry state
static bool s_isExpiredFailure = false;
static DiscoveredPeer s_lastFailedPeer = {};
static std::vector<std::wstring> s_lastFailedFiles;

// Radar animation & interaction
static float s_sonarRadius = 0.0f;
static int s_hoveredPeerIndex = -1;
static bool s_copyLinkHovered = false;
static bool s_fromTrayHovered = false;
static bool s_fromComputerHovered = false;
static bool s_closePickerHovered = false;
static bool s_sendAgainHovered = false;

// Hit test rectangles
static RECT s_copyLinkRect = {0, 0, 0, 0};
static std::vector<RECT> s_peerNodeRects;
static RECT s_fromTrayRect = {0, 0, 0, 0};
static RECT s_fromComputerRect = {0, 0, 0, 0};
static RECT s_closePickerRect = {0, 0, 0, 0};
static RECT s_sendAgainRect = {0, 0, 0, 0};
static RECT s_serviceToggleRect = {0, 0, 0, 0};
static RECT s_enableServiceBtnRect = {0, 0, 0, 0};
static bool s_serviceToggleHovered = false;
static bool s_enableServiceBtnHovered = false;

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

static std::wstring GetFileNameOnly(const std::wstring& path)
{
    size_t last = path.find_last_of(L"\\/");
    if (last != std::wstring::npos) return path.substr(last + 1);
    return path;
}

void SetShareActiveFile(const std::wstring& path)
{
    s_showSourcePicker = false;
    s_isExpiredFailure = false;
    s_lastFailedFiles.clear();
    if (path.empty()) {
        s_activeFilePaths.clear();
        s_statusMessage = L"Ready to share";
    } else {
        s_activeFilePaths = {path};
        s_statusMessage = L"File ready: " + GetFileNameOnly(path);
    }
}

int GetShareTabDesiredWidth(int dockEdge)
{
    if (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT)
        return 320;
    return 600;
}

int GetShareTabDesiredHeight(int dockEdge)
{
    if (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT)
        return 580;
    return 310;
}

static std::vector<std::wstring> PickFilesFromComputer(HWND hwnd)
{
    std::vector<std::wstring> result;
    std::vector<wchar_t> buffer(32768, 0);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        const wchar_t* p = buffer.data();
        std::wstring dir = p;
        p += dir.length() + 1;
        if (*p == L'\0') {
            // Single file selected
            result.push_back(dir);
        } else {
            // Multiple files selected
            while (*p != L'\0') {
                std::wstring fname = p;
                std::wstring fullPath = dir;
                if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
                fullPath += fname;
                result.push_back(fullPath);
                p += fname.length() + 1;
            }
        }
    }
    return result;
}

static void StartSendFiles(HWND hwnd, DiscoveredPeer targetPeer, const std::vector<std::wstring>& filePaths)
{
    if (filePaths.empty()) return;

    s_isTransferring = true;
    s_isExpiredFailure = false;
    s_lastFailedFiles.clear();

    std::wstring dispName = GetFileNameOnly(filePaths[0]);
    if (filePaths.size() > 1) {
        dispName += L" (+" + std::to_wstring(filePaths.size() - 1) + L" files)";
    }
    s_statusMessage = L"Requesting transfer to " + targetPeer.name + L"...";
    if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);

    std::thread([hwnd, targetPeer, filePaths, dispName]() {
        std::string err;
        bool expired = false;
        bool ok = NetworkManager::Instance().SendFilesToPeer(targetPeer.ip, filePaths, err, expired, targetPeer.port);
        s_isTransferring = false;
        if (ok) {
            s_statusMessage = L"Transfer to " + targetPeer.name + L" completed successfully!";
            s_isExpiredFailure = false;
            s_lastFailedFiles.clear();
            s_activeFilePaths.clear();
        } else {
            if (expired) {
                s_isExpiredFailure = true;
                s_lastFailedPeer = targetPeer;
                s_lastFailedFiles = filePaths;
                s_statusMessage = L"Transfer expired: " + targetPeer.name + L" did not respond.";
            } else {
                std::wstring wErr(err.begin(), err.end());
                s_statusMessage = L"Transfer failed: " + wErr;
                s_isExpiredFailure = false;
                s_lastFailedFiles.clear();
            }
            s_activeFilePaths.clear();
        }
        if (IsWindow(hwnd)) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }).detach();
}

static void DrawStandbyShareView(HDC hdc, int left, int top, int right, int bottom, bool isDark, bool isVert, HFONT titleFont, HFONT secFont, HFONT iconFont)
{
    int centerY = top + (bottom - top) / 2 - 12;
    int centerX = left + (right - left) / 2;

    int badgeD = 44;
    RECT badgeRc = {centerX - badgeD / 2, centerY - 52, centerX + badgeD / 2, centerY - 52 + badgeD};
    COLORREF badgeBg = isDark ? RGB(32, 38, 52) : RGB(225, 233, 246);
    COLORREF badgeBorder = isDark ? RGB(55, 65, 85) : RGB(195, 208, 226);
    DrawRoundedRect(hdc, badgeRc, badgeD / 2, badgeBg, badgeBorder, 1);

    SelectObject(hdc, iconFont);
    SetTextColor(hdc, isDark ? RGB(140, 155, 180) : RGB(70, 85, 110));
    DrawTextW(hdc, L"\xE704", -1, &badgeRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    RECT tRc = {left + 8, centerY, right - 8, centerY + 22};
    SelectObject(hdc, titleFont);
    SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(18, 22, 32));
    DrawTextW(hdc, L"WinShare is Turned Off", -1, &tRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    RECT sRc = {left + 14, centerY + 24, right - 14, centerY + 58};
    SelectObject(hdc, secFont);
    SetTextColor(hdc, isDark ? RGB(145, 155, 175) : RGB(65, 78, 98));
    DrawTextW(hdc, isVert ? L"Background peer discovery is stopped to save battery." : L"Background discovery & transfer server are stopped to eliminate background CPU and battery usage.", -1, &sRc, DT_WORDBREAK | DT_CENTER);

    int btnW = isVert ? 144 : 168;
    int btnH = 30;
    s_enableServiceBtnRect = {centerX - btnW / 2, centerY + 66, centerX + btnW / 2, centerY + 66 + btnH};
    COLORREF btnBg = s_enableServiceBtnHovered ? RGB(5, 150, 105) : RGB(16, 185, 129);
    DrawRoundedRect(hdc, s_enableServiceBtnRect, 8, btnBg, btnBg, 0);

    SelectObject(hdc, titleFont);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, L"\x26A1 Turn On WinShare", -1, &s_enableServiceBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
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
    HFONT fluentIconFont = CreateClearTypeFont(14, FW_NORMAL, L"Segoe Fluent Icons");
    HFONT titleFont = CreateClearTypeFont(13, FW_BOLD, L"Segoe UI");
    HFONT secFont = CreateClearTypeFont(11, FW_NORMAL, L"Segoe UI");
    HFONT boldSecFont = CreateClearTypeFont(11, FW_BOLD, L"Segoe UI");
    HFONT smallFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe UI");
    HGDIOBJ oldF = SelectObject(hdc, titleFont);
    SetBkMode(hdc, TRANSPARENT);

    std::string localIp = NetworkManager::Instance().GetLocalIP();
    int httpPort = NetworkManager::Instance().GetHttpPort();
    std::string qrUrl = "http://" + localIp + ":" + std::to_string(httpPort) + "/";
    std::wstring wUrl(qrUrl.begin(), qrUrl.end());

    EnsureQrCached(qrUrl);

    bool isSvcRunning = g_settings.enableAirDrop && NetworkManager::Instance().IsRunning();

    if (!isVert)
    {
        // ==========================================
        // HORIZONTAL DOCK SHARE TAB (EDGE_TOP)
        // ==========================================
        int contentTop = 38;
        int contentBottom = height - 12;

        // Service Toggle at Top-Right: Label + Fluent Toggle Switch
        int switchW = 38;
        int switchH = 20;
        int switchX = width - 16 - switchW;
        int switchY = contentTop + 1;
        RECT switchRc = {switchX, switchY, switchX + switchW, switchY + switchH};

        RECT lblRc = {switchX - 96, switchY, switchX - 8, switchY + switchH};
        SelectObject(hdc, smallFont);
        SetTextColor(hdc, isSvcRunning ? (isDark ? RGB(52, 211, 153) : RGB(5, 150, 105))
                                       : (isDark ? RGB(160, 170, 188) : RGB(70, 80, 100)));
        DrawTextW(hdc, isSvcRunning ? L"WinShare On" : L"WinShare Off", -1, &lblRc, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

        DrawToggleSwitch(hdc, switchRc, isSvcRunning, isDark, s_serviceToggleHovered);

        // Entire hit-test rectangle covering label and toggle switch
        s_serviceToggleRect = {lblRc.left, switchY - 2, switchRc.right + 2, switchY + switchH + 2};

        if (!isSvcRunning)
        {
            DrawStandbyShareView(hdc, 14, contentTop + 24, width - 14, contentBottom, isDark, false, titleFont, secFont, iconFont);
            RestoreDC(hdc, savedDC);
            DeleteObject(iconFont);
            DeleteObject(fluentIconFont);
            DeleteObject(titleFont);
            DeleteObject(secFont);
            DeleteObject(boldSecFont);
            DeleteObject(smallFont);
            return;
        }

        // --- LEFT PANE: Mobile Connect (QR Code) ---
        int leftCardW = 236;
        RECT leftCardRc = {14, contentTop, 14 + leftCardW, contentBottom};

        SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
        RECT qrTitleRc = {leftCardRc.left + 4, leftCardRc.top + 2, leftCardRc.right - 4, leftCardRc.top + 20};
        DrawTextW(hdc, L"Mobile Connect", -1, &qrTitleRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        SelectObject(hdc, secFont);
        SetTextColor(hdc, isDark ? RGB(150, 160, 180) : RGB(55, 68, 88));
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
        SetTextColor(hdc, isDark ? RGB(110, 122, 145) : RGB(65, 75, 95));
        DrawTextW(hdc, L"Same Wi-Fi (Set PC to Private Network)", -1, &hintRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // --- VERTICAL DIVIDER ---
        int divX = leftCardRc.right + 10;
        COLORREF divCol = isDark ? RGB(45, 52, 68) : RGB(190, 202, 218);
        HPEN divPen = CreatePen(PS_SOLID, 1, divCol);
        HGDIOBJ oldP = SelectObject(hdc, divPen);
        MoveToEx(hdc, divX, contentTop + 8, nullptr);
        LineTo(hdc, divX, contentBottom - 8);
        SelectObject(hdc, oldP);
        DeleteObject(divPen);

        // --- RIGHT PANE: Sonar Radar & Discovered PCs ---
        int rightLeft = divX + 12;
        int rightRight = width - 14;
        int rightWidth = rightRight - rightLeft;

        if (rightWidth > 60 && contentBottom > contentTop + 80)
        {
            SelectObject(hdc, titleFont);
            SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
            RECT radarTitleRc = {rightLeft + 4, contentTop + 2, rightLeft + 160, contentTop + 20};
            DrawTextW(hdc, L"Local Wi-Fi Radar", -1, &radarTitleRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            int bottomY = contentBottom - 36;
            // Radar center perfectly centered in right pane
            int radarCenterX = (rightLeft + rightRight) / 2;
            int radarCenterY = (contentTop + 26 + bottomY) / 2;

            // 3 Concentric circles (enlarged for pretty layout)
            COLORREF ringCol = isDark ? RGB(32, 44, 62) : RGB(210, 222, 238);
            HPEN ringPen = CreatePen(PS_SOLID, 1, ringCol);
            oldP = SelectObject(hdc, ringPen);
            HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));

            for (int r : {26, 54, 82}) {
                Ellipse(hdc, radarCenterX - r, radarCenterY - r, radarCenterX + r, radarCenterY + r);
            }

            // Animated continuous sonar wave ripples
            ULONGLONG nowTick = GetTickCount64();
            for (int offset : {0, 1100}) {
                float pNorm = static_cast<float>((nowTick + offset) % 2200) / 2200.0f;
                int pulseR = static_cast<int>(8.0f + pNorm * 74.0f);
                if (pulseR > 8 && pulseR < 84) {
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

            std::wstring myName = g_settings.username;
            if (myName.empty()) myName = L"You";
            std::wstring myLabel = myName + L" (You)";
            SelectObject(hdc, secFont);
            SIZE myTextSz = {};
            GetTextExtentPoint32W(hdc, myLabel.c_str(), static_cast<int>(myLabel.size()), &myTextSz);
            int myPillHalf = ClampInt(static_cast<int>(myTextSz.cx / 2) + 6, 20, 52);
            RECT youRc = {radarCenterX - myPillHalf, radarCenterY + 16, radarCenterX + myPillHalf, radarCenterY + 29};
            DrawRoundedRect(hdc, youRc, 4, isDark ? RGB(16, 20, 28) : RGB(242, 246, 252), isDark ? RGB(40, 48, 64) : RGB(215, 222, 235), 1);
            SetTextColor(hdc, isDark ? RGB(180, 195, 220) : RGB(30, 40, 60));
            DrawTextW(hdc, myLabel.c_str(), -1, &youRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            // Discovered Peer Nodes
            s_peerNodeRects.clear();
            std::vector<DiscoveredPeer> currentPeers;
            {
                std::lock_guard<std::mutex> lock(g_peersMutex);
                currentPeers = g_peers;
            }

            if (currentPeers.empty()) {
                SelectObject(hdc, secFont);
                SetTextColor(hdc, isDark ? RGB(110, 122, 142) : RGB(65, 75, 95));
                RECT searchRc = {rightLeft, radarCenterY + 44, rightLeft + rightWidth, radarCenterY + 60};
                DrawTextW(hdc, L"Scanning local Wi-Fi for nearby WinTray PCs...", -1, &searchRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            } else {
                for (size_t i = 0; i < currentPeers.size(); ++i) {
                    const auto& peer = currentPeers[i];
                    float angle;
                    if (currentPeers.size() == 1) {
                        angle = -0.785398f;
                    } else {
                        angle = static_cast<float>(i) * (6.283185f / currentPeers.size()) - 1.570796f;
                    }
                    float dist = 54.0f;

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
                    SetTextColor(hdc, isHovered ? RGB(0, 160, 255) : (isDark ? RGB(200, 210, 230) : RGB(25, 35, 55)));
                    DrawTextW(hdc, L"\xE7F8", -1, &nodeRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                    // Peer name label fitted snugly to text width (not overly wide)
                    SelectObject(hdc, boldSecFont);
                    SIZE peerTextSz = {};
                    GetTextExtentPoint32W(hdc, peer.name.c_str(), static_cast<int>(peer.name.size()), &peerTextSz);
                    int pillHalfW = ClampInt(static_cast<int>(peerTextSz.cx / 2) + 6, 18, 42);
                    RECT pNameRc = {px - pillHalfW, py + 16, px + pillHalfW, py + 29};
                    COLORREF nameBg = isDark ? RGB(16, 20, 28) : RGB(242, 246, 252);
                    DrawRoundedRect(hdc, pNameRc, 4, nameBg, isDark ? RGB(40, 48, 64) : RGB(215, 222, 235), 1);
                    SetTextColor(hdc, isDark ? RGB(235, 240, 252) : RGB(20, 30, 45));
                    DrawTextW(hdc, peer.name.c_str(), -1, &pNameRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
                }
            }

            // Bottom Area on Right Side: Source Picker OR Status / Send Again Bar
            bottomY = contentBottom - 36;
            if (s_showSourcePicker) {
                // Interactive Card: "Share to <PeerName>" with 2 options
                RECT pickerRc = {rightLeft + 4, bottomY - 6, rightLeft + rightWidth - 4, bottomY + 34};
                COLORREF pBg = isDark ? RGB(22, 28, 40) : RGB(236, 242, 252);
                DrawRoundedRect(hdc, pickerRc, 8, pBg, RGB(0, 140, 240), 1);

                // Option 1: [From Tray (N)]
                int optW = 105;
                s_fromTrayRect = {pickerRc.left + 8, pickerRc.top + 6, pickerRc.left + 8 + optW, pickerRc.bottom - 6};
                COLORREF trayBg = s_fromTrayHovered ? (isDark ? RGB(32, 90, 170) : RGB(190, 220, 255))
                                                    : (isDark ? RGB(24, 65, 120) : RGB(215, 232, 255));
                DrawRoundedRect(hdc, s_fromTrayRect, 6, trayBg, RGB(0, 140, 240), 1);
                SelectObject(hdc, boldSecFont);
                SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(0, 80, 180));
                std::wstring trayLabel = L"From Tray (" + std::to_wstring(g_items.size()) + L")";
                DrawTextW(hdc, trayLabel.c_str(), -1, &s_fromTrayRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                // Option 2: [From Computer...]
                s_fromComputerRect = {s_fromTrayRect.right + 6, pickerRc.top + 6, s_fromTrayRect.right + 6 + optW + 15, pickerRc.bottom - 6};
                COLORREF compBg = s_fromComputerHovered ? (isDark ? RGB(45, 55, 75) : RGB(220, 228, 242))
                                                        : (isDark ? RGB(30, 38, 54) : RGB(230, 238, 248));
                DrawRoundedRect(hdc, s_fromComputerRect, 6, compBg, isDark ? RGB(60, 75, 100) : RGB(190, 205, 225), 1);
                SelectObject(hdc, boldSecFont);
                SetTextColor(hdc, isDark ? RGB(220, 230, 250) : RGB(30, 45, 65));
                DrawTextW(hdc, L"From Computer...", -1, &s_fromComputerRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                // Close Button [X]
                s_closePickerRect = {pickerRc.right - 28, pickerRc.top + 6, pickerRc.right - 6, pickerRc.bottom - 6};
                SelectObject(hdc, fluentIconFont);
                SetTextColor(hdc, s_closePickerHovered ? RGB(239, 68, 68) : (isDark ? RGB(140, 150, 170) : RGB(120, 130, 145)));
                DrawTextW(hdc, L"\xE711", -1, &s_closePickerRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                s_sendAgainRect = {0, 0, 0, 0};
            } else {
                s_fromTrayRect = {0, 0, 0, 0};
                s_fromComputerRect = {0, 0, 0, 0};
                s_closePickerRect = {0, 0, 0, 0};

                if (s_isExpiredFailure && !s_lastFailedFiles.empty()) {
                    // Transfer Expired Bar with [Send Again?] Button
                    int btnW = 95;
                    RECT barRc = {rightLeft + 4, bottomY, rightLeft + rightWidth - btnW - 10, bottomY + 30};
                    COLORREF expBg = isDark ? RGB(40, 20, 24) : RGB(255, 232, 235);
                    DrawRoundedRect(hdc, barRc, 6, expBg, RGB(239, 68, 68), 1);
                    SelectObject(hdc, boldSecFont);
                    SetTextColor(hdc, RGB(239, 68, 68));
                    DrawTextW(hdc, L"\x23F1 AirDrop expired (Peer timed out)", -1, &barRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

                    s_sendAgainRect = {barRc.right + 6, bottomY, rightLeft + rightWidth - 4, bottomY + 30};
                    COLORREF sBg = s_sendAgainHovered ? RGB(5, 150, 105) : RGB(16, 185, 129);
                    DrawRoundedRect(hdc, s_sendAgainRect, 6, sBg, sBg, 0);
                    SelectObject(hdc, boldSecFont);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    DrawTextW(hdc, L"Send Again?", -1, &s_sendAgainRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                } else {
                    s_sendAgainRect = {0, 0, 0, 0};
                    RECT statusRc = {rightLeft + 4, bottomY, rightLeft + rightWidth - 4, bottomY + 30};
                    COLORREF sBg = isDark ? RGB(20, 24, 34) : RGB(235, 240, 248);
                    COLORREF sBorder = isDark ? RGB(45, 52, 68) : RGB(210, 218, 230);
                    DrawRoundedRect(hdc, statusRc, 6, sBg, sBorder, 1);

                    SelectObject(hdc, secFont);
                    SetTextColor(hdc, isDark ? RGB(180, 195, 220) : RGB(60, 75, 95));
                    std::wstring label = s_statusMessage;
                    if (label.empty() || label == L"Ready to share") {
                        label = L"Click on any nearby PC on the radar to send files";
                    }
                    RECT sTextRc = {statusRc.left + 8, statusRc.top, statusRc.right - 8, statusRc.bottom};
                    DrawTextW(hdc, label.c_str(), -1, &sTextRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
                }
            }
        }
    }
    else
    {
        // ==========================================
        // VERTICAL DOCK SHARE TAB (EDGE_LEFT / RIGHT)
        // ==========================================
        bool isLeft = (dockEdge == EDGE_LEFT);
        int contentLeft = isLeft ? 42 : 12;
        int contentRight = isLeft ? (width - 12) : (width - 42);
        int contentW = contentRight - contentLeft;
        int topY = 20;

        // Service Toggle at Top-Right
        int switchW = 36;
        int switchH = 18;
        int switchX = contentRight - switchW;
        int switchY = topY + 1;
        RECT switchRc = {switchX, switchY, switchX + switchW, switchY + switchH};

        DrawToggleSwitch(hdc, switchRc, isSvcRunning, isDark, s_serviceToggleHovered);

        s_serviceToggleRect = {switchX - 4, switchY - 2, switchRc.right + 2, switchY + switchH + 2};

        if (!isSvcRunning)
        {
            DrawStandbyShareView(hdc, contentLeft, topY + 28, contentRight, height - 16, isDark, true, titleFont, secFont, iconFont);
            RestoreDC(hdc, savedDC);
            DeleteObject(iconFont);
            DeleteObject(fluentIconFont);
            DeleteObject(titleFont);
            DeleteObject(secFont);
            DeleteObject(boldSecFont);
            DeleteObject(smallFont);
            return;
        }

        SelectObject(hdc, titleFont);
        SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
        RECT qrTitleRc = {contentLeft, topY, contentRight - 60, topY + 20};
        DrawTextW(hdc, L"AirDrop / Share", -1, &qrTitleRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        // Enlarged QR Code (152px)
        int qrCardSize = 152;
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
        s_copyLinkRect = {contentLeft + 8, linkY, contentRight - 8, linkY + 22};
        COLORREF linkBg = s_copyLinkHovered ? (isDark ? RGB(20, 50, 95) : RGB(215, 235, 255))
                                            : (isDark ? RGB(24, 30, 42) : RGB(230, 236, 246));
        DrawRoundedRect(hdc, s_copyLinkRect, 6, linkBg, RGB(0, 120, 215), 1);
        SelectObject(hdc, boldSecFont);
        SetTextColor(hdc, s_copyLinkHovered ? RGB(0, 160, 255) : (isDark ? RGB(100, 180, 255) : RGB(0, 110, 210)));
        DrawTextW(hdc, wUrl.c_str(), -1, &s_copyLinkRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // Divider
        int divY = linkY + 28;
        COLORREF divCol = isDark ? RGB(45, 52, 68) : RGB(190, 202, 218);
        HPEN divPen = CreatePen(PS_SOLID, 1, divCol);
        HGDIOBJ oldP = SelectObject(hdc, divPen);
        MoveToEx(hdc, contentLeft + 10, divY, nullptr);
        LineTo(hdc, contentRight - 10, divY);
        SelectObject(hdc, oldP);
        DeleteObject(divPen);

        if (height > 320)
        {
            int bAreaY = height - 76;
            // Radar center perfectly centered in the vertical content pane
            int radarCenterX = (contentLeft + contentRight) / 2;
            int radarCenterY = (divY + 12 + bAreaY - 8) / 2;

            COLORREF ringCol = isDark ? RGB(32, 44, 62) : RGB(210, 222, 238);
            HPEN ringPen = CreatePen(PS_SOLID, 1, ringCol);
            oldP = SelectObject(hdc, ringPen);
            HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));

            for (int r : {26, 54, 82}) {
                Ellipse(hdc, radarCenterX - r, radarCenterY - r, radarCenterX + r, radarCenterY + r);
            }

            ULONGLONG nowTick = GetTickCount64();
            for (int offset : {0, 1100}) {
                float pNorm = static_cast<float>((nowTick + offset) % 2200) / 2200.0f;
                int pulseR = static_cast<int>(8.0f + pNorm * 74.0f);
                if (pulseR > 8 && pulseR < 84) {
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

            std::wstring myName = g_settings.username;
            if (myName.empty()) myName = L"You";
            std::wstring myLabel = myName + L" (You)";
            SelectObject(hdc, secFont);
            SIZE myTextSz = {};
            GetTextExtentPoint32W(hdc, myLabel.c_str(), static_cast<int>(myLabel.size()), &myTextSz);
            int myPillHalf = ClampInt(static_cast<int>(myTextSz.cx / 2) + 6, 20, 52);
            RECT youRc = {radarCenterX - myPillHalf, radarCenterY + 16, radarCenterX + myPillHalf, radarCenterY + 29};
            DrawRoundedRect(hdc, youRc, 4, isDark ? RGB(16, 20, 28) : RGB(242, 246, 252), isDark ? RGB(40, 48, 64) : RGB(215, 222, 235), 1);
            SetTextColor(hdc, isDark ? RGB(180, 195, 220) : RGB(30, 40, 60));
            DrawTextW(hdc, myLabel.c_str(), -1, &youRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            // Discovered Peer Nodes in vertical mode
            s_peerNodeRects.clear();
            std::vector<DiscoveredPeer> currentPeers;
            {
                std::lock_guard<std::mutex> lock(g_peersMutex);
                currentPeers = g_peers;
            }

            for (size_t i = 0; i < currentPeers.size(); ++i) {
                const auto& peer = currentPeers[i];
                float angle;
                if (currentPeers.size() == 1) {
                    angle = -0.785398f;
                } else {
                    angle = static_cast<float>(i) * (6.283185f / currentPeers.size()) - 1.570796f;
                }
                float dist = 54.0f;

                int px = radarCenterX + static_cast<int>(dist * cos(angle));
                int py = radarCenterY + static_cast<int>(dist * sin(angle));

                RECT nodeRc = {px - 14, py - 14, px + 14, py + 14};
                s_peerNodeRects.push_back(nodeRc);

                bool isHovered = (s_hoveredPeerIndex == static_cast<int>(i));
                COLORREF nBg = isHovered ? (isDark ? RGB(20, 65, 125) : RGB(180, 215, 255))
                                         : (isDark ? RGB(28, 35, 48) : RGB(225, 235, 248));
                DrawRoundedRect(hdc, nodeRc, 14, nBg, isHovered ? RGB(0, 160, 255) : RGB(70, 85, 110), 1);

                SelectObject(hdc, iconFont);
                SetTextColor(hdc, isHovered ? RGB(0, 160, 255) : (isDark ? RGB(200, 210, 230) : RGB(25, 35, 55)));
                DrawTextW(hdc, L"\xE7F8", -1, &nodeRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                // Device name badge fitted snugly to text width (not overly wide)
                SelectObject(hdc, boldSecFont);
                SIZE peerTextSz = {};
                GetTextExtentPoint32W(hdc, peer.name.c_str(), static_cast<int>(peer.name.size()), &peerTextSz);
                int pillHalfW = ClampInt(static_cast<int>(peerTextSz.cx / 2) + 6, 18, 42);
                RECT pNameRc = {px - pillHalfW, py + 16, px + pillHalfW, py + 29};
                COLORREF nameBg = isDark ? RGB(16, 20, 28) : RGB(242, 246, 252);
                DrawRoundedRect(hdc, pNameRc, 4, nameBg, isDark ? RGB(40, 48, 64) : RGB(215, 222, 235), 1);
                SetTextColor(hdc, isDark ? RGB(235, 240, 252) : RGB(20, 30, 45));
                DrawTextW(hdc, peer.name.c_str(), -1, &pNameRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
            }

            // Bottom Area in Vertical Dock
            bAreaY = height - 76;
            if (s_showSourcePicker) {
                RECT pickerRc = {contentLeft + 4, bAreaY - 8, contentRight - 4, height - 10};
                COLORREF pBg = isDark ? RGB(22, 28, 40) : RGB(236, 242, 252);
                DrawRoundedRect(hdc, pickerRc, 8, pBg, RGB(0, 140, 240), 1);

                // Option 1: [From Tray]
                int halfW = (pickerRc.right - pickerRc.left - 20) / 2;
                s_fromTrayRect = {pickerRc.left + 6, pickerRc.top + 6, pickerRc.left + 6 + halfW, pickerRc.bottom - 6};
                COLORREF trayBg = s_fromTrayHovered ? (isDark ? RGB(32, 90, 170) : RGB(190, 220, 255))
                                                    : (isDark ? RGB(24, 65, 120) : RGB(215, 232, 255));
                DrawRoundedRect(hdc, s_fromTrayRect, 6, trayBg, RGB(0, 140, 240), 1);
                SelectObject(hdc, boldSecFont);
                SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(0, 80, 180));
                std::wstring tLabel = L"From Tray (" + std::to_wstring(g_items.size()) + L")";
                DrawTextW(hdc, tLabel.c_str(), -1, &s_fromTrayRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                // Option 2: [Computer...]
                s_fromComputerRect = {s_fromTrayRect.right + 6, pickerRc.top + 6, pickerRc.right - 26, pickerRc.bottom - 6};
                COLORREF compBg = s_fromComputerHovered ? (isDark ? RGB(45, 55, 75) : RGB(220, 228, 242))
                                                        : (isDark ? RGB(30, 38, 54) : RGB(230, 238, 248));
                DrawRoundedRect(hdc, s_fromComputerRect, 6, compBg, isDark ? RGB(60, 75, 100) : RGB(190, 205, 225), 1);
                SelectObject(hdc, boldSecFont);
                SetTextColor(hdc, isDark ? RGB(220, 230, 250) : RGB(30, 45, 65));
                DrawTextW(hdc, L"Computer...", -1, &s_fromComputerRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                // Close Button [X]
                s_closePickerRect = {pickerRc.right - 24, pickerRc.top + 6, pickerRc.right - 4, pickerRc.bottom - 6};
                SelectObject(hdc, fluentIconFont);
                SetTextColor(hdc, s_closePickerHovered ? RGB(239, 68, 68) : (isDark ? RGB(140, 150, 170) : RGB(120, 130, 145)));
                DrawTextW(hdc, L"\xE711", -1, &s_closePickerRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                s_sendAgainRect = {0, 0, 0, 0};
            } else {
                s_fromTrayRect = {0, 0, 0, 0};
                s_fromComputerRect = {0, 0, 0, 0};
                s_closePickerRect = {0, 0, 0, 0};

                if (s_isExpiredFailure && !s_lastFailedFiles.empty()) {
                    // Send Again Button
                    s_sendAgainRect = {contentLeft + 8, height - 42, contentLeft + contentW - 8, height - 12};
                    COLORREF sBg = s_sendAgainHovered ? RGB(5, 150, 105) : RGB(16, 185, 129);
                    DrawRoundedRect(hdc, s_sendAgainRect, 6, sBg, sBg, 0);
                    SelectObject(hdc, boldSecFont);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    DrawTextW(hdc, L"AirDrop Expired. Send Again?", -1, &s_sendAgainRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                } else {
                    s_sendAgainRect = {0, 0, 0, 0};
                    RECT statusRc = {contentLeft + 8, height - 42, contentLeft + contentW - 8, height - 12};
                    COLORREF sBg = isDark ? RGB(20, 24, 34) : RGB(235, 240, 248);
                    COLORREF sBorder = isDark ? RGB(45, 52, 68) : RGB(210, 218, 230);
                    DrawRoundedRect(hdc, statusRc, 6, sBg, sBorder, 1);
                    SelectObject(hdc, secFont);
                    SetTextColor(hdc, isDark ? RGB(180, 195, 220) : RGB(60, 75, 95));
                    std::wstring label = s_statusMessage;
                    if (label.empty() || label == L"Ready to share") {
                        label = L"Click nearby PC to send";
                    }
                    DrawTextW(hdc, label.c_str(), -1, &statusRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
                }
            }
        }
    }

    SelectObject(hdc, oldF);
    DeleteObject(iconFont);
    DeleteObject(fluentIconFont);
    DeleteObject(titleFont);
    DeleteObject(secFont);
    DeleteObject(boldSecFont);
    DeleteObject(smallFont);
    RestoreDC(hdc, savedDC);
}

bool HandleShareTabMouseMove(HWND hwnd, POINT pt)
{
    bool needRepaint = false;

    bool togHov = PtInRect(&s_serviceToggleRect, pt);
    if (togHov != s_serviceToggleHovered) {
        s_serviceToggleHovered = togHov;
        needRepaint = true;
    }
    bool enHov = PtInRect(&s_enableServiceBtnRect, pt);
    if (enHov != s_enableServiceBtnHovered) {
        s_enableServiceBtnHovered = enHov;
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

    bool trayHov = PtInRect(&s_fromTrayRect, pt);
    if (trayHov != s_fromTrayHovered) {
        s_fromTrayHovered = trayHov;
        needRepaint = true;
    }

    bool compHov = PtInRect(&s_fromComputerRect, pt);
    if (compHov != s_fromComputerHovered) {
        s_fromComputerHovered = compHov;
        needRepaint = true;
    }

    bool closeHov = PtInRect(&s_closePickerRect, pt);
    if (closeHov != s_closePickerHovered) {
        s_closePickerHovered = closeHov;
        needRepaint = true;
    }

    bool againHov = PtInRect(&s_sendAgainRect, pt);
    if (againHov != s_sendAgainHovered) {
        s_sendAgainHovered = againHov;
        needRepaint = true;
    }

    if (needRepaint && IsWindow(hwnd)) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return (linkHov || peerHov != -1 || trayHov || compHov || closeHov || againHov || togHov || enHov);
}

bool HandleShareTabClick(HWND hwnd, POINT pt)
{
    // 0. Toggle WinShare Service ON/OFF
    if (PtInRect(&s_serviceToggleRect, pt) || PtInRect(&s_enableServiceBtnRect, pt)) {
        bool newActive = !g_settings.enableAirDrop;
        if (PtInRect(&s_enableServiceBtnRect, pt)) {
            newActive = true;
        }
        g_settings.enableAirDrop = newActive;
        SaveSettings();
        if (newActive) {
            NetworkManager::Instance().Start();
            NetworkManager::Instance().TriggerQuickScan();
        } else {
            NetworkManager::Instance().Stop();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    if (!g_settings.enableAirDrop || !NetworkManager::Instance().IsRunning()) {
        return false;
    }

    // 1. Click Copy Link
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

    // 2. Click "Send Again?" button if transfer expired
    if (PtInRect(&s_sendAgainRect, pt)) {
        if (s_isExpiredFailure && !s_lastFailedFiles.empty()) {
            StartSendFiles(hwnd, s_lastFailedPeer, s_lastFailedFiles);
        }
        return true;
    }

    // 3. Source Picker Actions
    if (s_showSourcePicker) {
        if (PtInRect(&s_fromTrayRect, pt)) {
            s_showSourcePicker = false;
            std::vector<std::wstring> trayFiles;
            for (const auto& it : g_items) {
                if (it.isSelected) trayFiles.push_back(it.path);
            }
            if (trayFiles.empty()) {
                for (const auto& it : g_items) trayFiles.push_back(it.path);
            }
            if (!trayFiles.empty()) {
                StartSendFiles(hwnd, s_chosenPeer, trayFiles);
            } else {
                s_statusMessage = L"Tray is empty.";
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return true;
        }

        if (PtInRect(&s_fromComputerRect, pt)) {
            s_showSourcePicker = false;
            auto pickedFiles = PickFilesFromComputer(hwnd);
            if (!pickedFiles.empty()) {
                StartSendFiles(hwnd, s_chosenPeer, pickedFiles);
            }
            return true;
        }

        if (PtInRect(&s_closePickerRect, pt)) {
            s_showSourcePicker = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
    }

    // 4. Click on Discovered Peer Node
    for (size_t i = 0; i < s_peerNodeRects.size(); ++i) {
        if (PtInRect(&s_peerNodeRects[i], pt)) {
            if (s_isTransferring) return true;

            DiscoveredPeer targetPeer;
            {
                std::lock_guard<std::mutex> lock(g_peersMutex);
                if (i < g_peers.size()) targetPeer = g_peers[i];
                else return true;
            }

            // If a specific file was passed via right-click (s_activeFilePaths not empty)
            if (!s_activeFilePaths.empty()) {
                std::vector<std::wstring> files = s_activeFilePaths;
                s_activeFilePaths.clear();
                StartSendFiles(hwnd, targetPeer, files);
                return true;
            }

            // Requirement 4: If tray is empty, automatically open computer picker.
            // If tray has files, show the 2 options: "From tray" and "From computer".
            if (g_items.empty()) {
                auto pickedFiles = PickFilesFromComputer(hwnd);
                if (!pickedFiles.empty()) {
                    StartSendFiles(hwnd, targetPeer, pickedFiles);
                }
            } else {
                s_showSourcePicker = true;
                s_chosenPeer = targetPeer;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return true;
        }
    }

    // Click outside dismisses source picker
    if (s_showSourcePicker) {
        s_showSourcePicker = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    return false;
}

// Standalone dialog or dock switch
void OpenShareDialog(HWND parentHwnd, const std::wstring &initialFilePath)
{
    SetShareActiveFile(initialFilePath);

    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        g_activeTab = TAB_SHARE;
        if (!g_settings.enableAirDrop) {
            g_settings.enableAirDrop = true;
            SaveSettings();
            NetworkManager::Instance().Start();
        }
        NetworkManager::Instance().TriggerQuickScan();
        TriggerExpand(g_mainHwnd);
        CalculateTargetSize(true);
        SetTimer(g_mainHwnd, ANIM_TIMER_ID, 12, nullptr);
        SetForegroundWindow(g_mainHwnd);
        InvalidateRect(g_mainHwnd, nullptr, TRUE);
    }
}
