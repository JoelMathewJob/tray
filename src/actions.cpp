#include "actions.h"
#include "player.h"
#include "state.h"
#include "settings.h"
#include <shlobj.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <algorithm>

ScreenRecordingState g_recording = {};
ActionToast g_toast = {};

// Row 1: The 3 Core Action Buttons
RECT g_actionRecBtnRect = {};
RECT g_actionSnipBtnRect = {};
RECT g_actionShotBtnRect = {};

// Row 2: Utility Action Buttons
RECT g_actionFolderBtnRect = {};
RECT g_actionClipboardBtnRect = {};
RECT g_actionLockBtnRect = {};

// Hover States
bool g_actionRecHovered = false;
bool g_actionSnipHovered = false;
bool g_actionShotHovered = false;
bool g_actionFolderHovered = false;
bool g_actionClipboardHovered = false;
bool g_actionLockHovered = false;

static IAudioMeterInformation *s_pAudioMeter = nullptr;
static float s_smoothedPeak = 0.0f;

// Helper: Create ClearType Font
static HFONT CreateClearTypeFont(int sizePx, int weight, const wchar_t *fontFamily = L"Segoe UI")
{
    return CreateFontW(-sizePx, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fontFamily);
}

// Helper: Draw Rounded Rectangle
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

// Notification Toast Trigger
void ShowActionToast(const std::wstring &msg, const wchar_t *glyph, COLORREF iconCol)
{
    g_toast.active = true;
    g_toast.message = msg;
    g_toast.iconGlyph = glyph ? glyph : L"\xE73E";
    g_toast.iconColor = iconCol;
    g_toast.showTime = GetTickCount64();

    MessageBeep(MB_OK);

    if (g_mainHwnd && IsWindow(g_mainHwnd))
    {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

// Notification Toast Renderer
void RenderActionToast(HDC hdc, int width, int height, bool isDark)
{
    if (!g_toast.active)
        return;

    ULONGLONG elapsed = GetTickCount64() - g_toast.showTime;
    if (elapsed > 3500)
    {
        g_toast.active = false;
        return;
    }

    HFONT tFont = CreateClearTypeFont(11, FW_SEMIBOLD, L"Segoe UI");
    HFONT iFont = CreateClearTypeFont(12, FW_NORMAL, L"Segoe MDL2 Assets");
    HGDIOBJ oldF = SelectObject(hdc, tFont);

    SIZE sz = {};
    GetTextExtentPoint32W(hdc, g_toast.message.c_str(), static_cast<int>(g_toast.message.size()), &sz);

    int pillW = sz.cx + 52;
    int pillH = 30;
    int pillX = (width - pillW) / 2;
    int pillY = 8; // Float near top edge of dock

    RECT toastRc = {pillX, pillY, pillX + pillW, pillY + pillH};
    COLORREF bg = isDark ? RGB(20, 24, 34) : RGB(255, 255, 255);
    COLORREF border = g_toast.iconColor;
    DrawRoundedRect(hdc, toastRc, 14, bg, border, 1);

    // Icon
    SelectObject(hdc, iFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_toast.iconColor);
    RECT iconRc = {toastRc.left + 8, toastRc.top, toastRc.left + 28, toastRc.bottom};
    DrawTextW(hdc, g_toast.iconGlyph.c_str(), -1, &iconRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    // Text
    SelectObject(hdc, tFont);
    SetTextColor(hdc, isDark ? RGB(245, 248, 255) : RGB(18, 22, 32));
    RECT textRc = {toastRc.left + 30, toastRc.top, toastRc.right - 8, toastRc.bottom};
    DrawTextW(hdc, g_toast.message.c_str(), -1, &textRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    SelectObject(hdc, oldF);
    DeleteObject(tFont);
    DeleteObject(iFont);
}

// Helper: Locate FFmpeg Binary
static std::wstring FindFFmpegExecutable()
{
    wchar_t foundPath[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, foundPath, nullptr) > 0)
    {
        return foundPath;
    }

    wchar_t *localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
    {
        std::wstring baseDir = std::wstring(localAppData) + L"\\Microsoft\\WinGet\\Packages\\";
        CoTaskMemFree(localAppData);

        WIN32_FIND_DATAW findData = {};
        HANDLE hFind = FindFirstFileW((baseDir + L"*ffmpeg*").c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    std::wstring candidate = baseDir + findData.cFileName + L"\\bin\\ffmpeg.exe";
                    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        FindClose(hFind);
                        return candidate;
                    }

                    WIN32_FIND_DATAW subFind = {};
                    HANDLE hSub = FindFirstFileW((baseDir + findData.cFileName + L"\\*").c_str(), &subFind);
                    if (hSub != INVALID_HANDLE_VALUE)
                    {
                        do
                        {
                            if (subFind.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                            {
                                std::wstring nested = baseDir + findData.cFileName + L"\\" + subFind.cFileName + L"\\bin\\ffmpeg.exe";
                                if (GetFileAttributesW(nested.c_str()) != INVALID_FILE_ATTRIBUTES)
                                {
                                    FindClose(hSub);
                                    FindClose(hFind);
                                    return nested;
                                }
                            }
                        } while (FindNextFileW(hSub, &subFind));
                        FindClose(hSub);
                    }
                }
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }
    }

    return L"ffmpeg.exe";
}

static std::wstring GetCapturesDirectory()
{
    wchar_t *pVideos = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &pVideos)))
    {
        std::wstring dir = std::wstring(pVideos) + L"\\Captures";
        CoTaskMemFree(pVideos);
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return L".";
}

static std::wstring GetScreenshotsDirectory()
{
    wchar_t *pPictures = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pPictures)))
    {
        std::wstring dir = std::wstring(pPictures) + L"\\Screenshots";
        CoTaskMemFree(pPictures);
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return L".";
}

static std::wstring GetTimestampString()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

static int GetEncoderClsid(const WCHAR *format, CLSID *pClsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    auto *pImageCodecInfo = static_cast<Gdiplus::ImageCodecInfo *>(malloc(size));
    if (!pImageCodecInfo) return -1;
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j)
    {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
        {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

void InitActionsEngine()
{
    if (!s_pAudioMeter)
    {
        IMMDeviceEnumerator *pEnum = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void **)&pEnum)))
        {
            IMMDevice *pDevice = nullptr;
            if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice)))
            {
                pDevice->Activate(__uuidof(IAudioMeterInformation), CLSCTX_INPROC_SERVER, nullptr, (void **)&s_pAudioMeter);
                pDevice->Release();
            }
            pEnum->Release();
        }
    }

    // Set Window Display Affinity so the tray dock is natively hidden from all screen recording & capture APIs
    if (g_mainHwnd && IsWindow(g_mainHwnd))
    {
        SetWindowDisplayAffinity(g_mainHwnd, 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
    }
}

void ShutdownActionsEngine()
{
    if (g_recording.isRecording)
    {
        StopScreenRecording(nullptr);
    }
    if (s_pAudioMeter)
    {
        s_pAudioMeter->Release();
        s_pAudioMeter = nullptr;
    }
}

float GetSystemAudioPeak()
{
    if (!s_pAudioMeter)
    {
        InitActionsEngine();
    }
    float rawPeak = 0.0f;
    if (s_pAudioMeter && SUCCEEDED(s_pAudioMeter->GetPeakValue(&rawPeak)))
    {
        s_smoothedPeak = s_smoothedPeak * 0.7f + rawPeak * 0.3f;
    }
    else
    {
        s_smoothedPeak *= 0.85f;
    }
    return s_smoothedPeak;
}

std::wstring GetRecordingDurationString()
{
    if (!g_recording.isRecording)
        return L"00:00";

    ULONGLONG elapsedMs = GetTickCount64() - g_recording.startTime;
    int totalSec = static_cast<int>(elapsedMs / 1000);
    int minutes = totalSec / 60;
    int seconds = totalSec % 60;

    wchar_t buf[32];
    if (minutes >= 60)
    {
        int hours = minutes / 60;
        minutes %= 60;
        swprintf_s(buf, L"%02d:%02d:%02d", hours, minutes, seconds);
    }
    else
    {
        swprintf_s(buf, L"%02d:%02d", minutes, seconds);
    }
    return buf;
}

bool StartScreenRecording(HWND hwnd)
{
    if (g_recording.isRecording)
        return true;

    // Ensure tray is excluded from screen capture
    if (hwnd && IsWindow(hwnd))
    {
        SetWindowDisplayAffinity(hwnd, 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
    }

    std::wstring ffmpegPath = FindFFmpegExecutable();
    std::wstring outDir = GetCapturesDirectory();
    std::wstring outPath = outDir + L"\\WinTray_Rec_" + GetTimestampString() + L".mp4";

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hChildStdInRead = nullptr;
    HANDLE hChildStdInWrite = nullptr;

    if (!CreatePipe(&hChildStdInRead, &hChildStdInWrite, &sa, 0))
        return false;
    SetHandleInformation(hChildStdInWrite, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + ffmpegPath + L"\" -y -f gdigrab -framerate 30 -draw_mouse 1 -i desktop -c:v libx264 -preset ultrafast -crf 23 -pix_fmt yuv420p \"" + outPath + L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdInput = hChildStdInRead;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    BOOL success = CreateProcessW(
        nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(hChildStdInRead);

    if (success)
    {
        CloseHandle(pi.hThread);
        g_recording.isRecording = true;
        g_recording.startTime = GetTickCount64();
        g_recording.outputPath = outPath;
        g_recording.hProcess = pi.hProcess;
        g_recording.hStdinWrite = hChildStdInWrite;

        ShowActionToast(L"Recording Started! (Tray excluded from video)", L"\xE7C8", RGB(239, 68, 68));

        if (hwnd && IsWindow(hwnd))
        {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }
    else
    {
        CloseHandle(hChildStdInWrite);
        return false;
    }
}

bool StopScreenRecording(HWND hwnd)
{
    if (!g_recording.isRecording)
        return false;

    if (g_recording.hStdinWrite)
    {
        DWORD written = 0;
        WriteFile(g_recording.hStdinWrite, "q\n", 2, &written, nullptr);
        CloseHandle(g_recording.hStdinWrite);
        g_recording.hStdinWrite = nullptr;
    }

    if (g_recording.hProcess)
    {
        DWORD waitRes = WaitForSingleObject(g_recording.hProcess, 3000);
        if (waitRes == WAIT_TIMEOUT)
        {
            TerminateProcess(g_recording.hProcess, 0);
        }
        CloseHandle(g_recording.hProcess);
        g_recording.hProcess = nullptr;
    }

    g_recording.isRecording = false;

    if (GetFileAttributesW(g_recording.outputPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (g_settings.recAutoDropInTray)
        {
            AddFileToTray(g_recording.outputPath, false);
        }
        ShowActionToast(L"Recording Saved to Videos\\Captures!", L"\xE71A", RGB(239, 68, 68));
    }

    if (hwnd && IsWindow(hwnd))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return true;
}

void ToggleScreenRecording(HWND hwnd)
{
    if (g_recording.isRecording)
    {
        StopScreenRecording(hwnd);
    }
    else
    {
        StartScreenRecording(hwnd);
    }
}

bool CaptureInstantScreenshot(HWND hwnd)
{
    // Temporarily hide the tray window so it is 100% invisible in the screenshot
    if (hwnd && IsWindow(hwnd))
    {
        ShowWindow(hwnd, SW_HIDE);
        Sleep(50); // Allow DWM to composite desktop without tray
    }

    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hScreenDC = GetDC(nullptr);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreenDC, screenW, screenH);
    HGDIOBJ oldB = SelectObject(hMemDC, hBmp);

    BitBlt(hMemDC, 0, 0, screenW, screenH, hScreenDC, screenX, screenY, SRCCOPY | CAPTUREBLT);

    // Restore tray window visibility
    if (hwnd && IsWindow(hwnd))
    {
        ShowWindow(hwnd, SW_SHOW);
    }

    // Save to PNG using GDI+
    Gdiplus::Bitmap gdiBmp(hBmp, nullptr);
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) >= 0)
    {
        std::wstring outDir = GetScreenshotsDirectory();
        std::wstring outPath = outDir + L"\\WinTray_Shot_" + GetTimestampString() + L".png";
        gdiBmp.Save(outPath.c_str(), &pngClsid, nullptr);

        if (OpenClipboard(hwnd))
        {
            EmptyClipboard();
            SetClipboardData(CF_BITMAP, hBmp);
            CloseClipboard();
        }

        AddFileToTray(outPath, false);
        ShowActionToast(L"Screenshot Saved & Copied to Clipboard!", L"\xE7F4", RGB(34, 197, 94));
    }

    SelectObject(hMemDC, oldB);
    DeleteObject(hBmp);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);

    if (hwnd && IsWindow(hwnd))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return true;
}

bool LaunchRegionSnippingTool()
{
    // Temporarily hide the tray window so it is not visible in the snip region
    if (g_mainHwnd && IsWindow(g_mainHwnd))
    {
        ShowWindow(g_mainHwnd, SW_HIDE);
    }

    HINSTANCE hRes = ShellExecuteW(nullptr, L"open", L"ms-screenclip:", nullptr, nullptr, SW_SHOWNORMAL);

    // Restore dock after user starts snipping
    if (g_mainHwnd && IsWindow(g_mainHwnd))
    {
        ShowWindow(g_mainHwnd, SW_SHOW);
        ShowActionToast(L"Snipping Tool Launched", L"\xE8C6", RGB(2, 132, 199));
    }
    return (reinterpret_cast<INT_PTR>(hRes) > 32);
}

// =======================================================================
// Collapsed View Renderer (Recording HUD / Clock / Visualizer / Pill)
// =======================================================================
void RenderCollapsedDockContent(HDC hdc, int width, int height, bool isDark, bool isVert)
{
    SetBkMode(hdc, TRANSPARENT);

    // 1. TOP PRIORITY: Screen Recording Active HUD
    if (g_recording.isRecording)
    {
        std::wstring durStr = GetRecordingDurationString();
        ULONGLONG now = GetTickCount64();
        bool blink = ((now / 450) % 2 == 0);

        if (!isVert)
        {
            // Horizontal Recording HUD
            // Left: Timer String
            HFONT timerFont = CreateClearTypeFont(11, FW_BOLD, L"Segoe UI");
            HGDIOBJ oldF = SelectObject(hdc, timerFont);
            SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
            RECT timerRc = {14, 0, 75, height};
            DrawTextW(hdc, durStr.c_str(), -1, &timerRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            // Center: Minimalist Grab Line
            int pillW = 28;
            int pillH = 3;
            int pillX = (width - pillW) / 2;
            int pillY = (height - pillH) / 2;
            COLORREF cLine = isDark ? RGB(80, 88, 105) : RGB(140, 150, 168);
            HBRUSH lBr = CreateSolidBrush(cLine);
            HPEN noP = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, lBr);
            SelectObject(hdc, noP);
            RoundRect(hdc, pillX, pillY, pillX + pillW, pillY + pillH, 3, 3);
            DeleteObject(lBr);

            // Right: Pulsating Red Dot + "REC"
            int dotX = width - 48;
            int dotY = height / 2;
            int dotR = 4;
            COLORREF redCol = blink ? RGB(245, 55, 55) : RGB(160, 25, 25);
            HBRUSH redBr = CreateSolidBrush(redCol);
            SelectObject(hdc, redBr);
            RoundRect(hdc, dotX - dotR, dotY - dotR, dotX + dotR, dotY + dotR, dotR * 2, dotR * 2);
            DeleteObject(redBr);
            DeleteObject(noP);

            HFONT recFont = CreateClearTypeFont(10, FW_BOLD, L"Segoe UI");
            SelectObject(hdc, recFont);
            SetTextColor(hdc, redCol);
            RECT recRc = {dotX + 6, 0, width - 10, height};
            DrawTextW(hdc, L"REC", -1, &recRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            SelectObject(hdc, oldF);
            DeleteObject(timerFont);
            DeleteObject(recFont);
        }
        else
        {
            // Vertical Recording HUD
            int dotX = width / 2;
            int dotY = 16;
            int dotR = 4;
            COLORREF redCol = blink ? RGB(245, 55, 55) : RGB(160, 25, 25);
            HBRUSH redBr = CreateSolidBrush(redCol);
            HPEN noP = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ oldB = SelectObject(hdc, redBr);
            HGDIOBJ oldP = SelectObject(hdc, noP);
            RoundRect(hdc, dotX - dotR, dotY - dotR, dotX + dotR, dotY + dotR, dotR * 2, dotR * 2);
            DeleteObject(redBr);
            DeleteObject(noP);

            HFONT recFont = CreateClearTypeFont(9, FW_BOLD, L"Segoe UI");
            HGDIOBJ oldF = SelectObject(hdc, recFont);
            SetTextColor(hdc, redCol);
            RECT recRc = {0, 24, width, 40};
            DrawTextW(hdc, L"REC", -1, &recRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            HFONT timerFont = CreateClearTypeFont(9, FW_BOLD, L"Segoe UI");
            SelectObject(hdc, timerFont);
            SetTextColor(hdc, isDark ? RGB(255, 255, 255) : RGB(20, 24, 32));
            RECT timerRc = {0, height - 34, width, height - 10};
            DrawTextW(hdc, durStr.c_str(), -1, &timerRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            SelectObject(hdc, oldF);
            DeleteObject(timerFont);
            DeleteObject(recFont);
        }
        return;
    }

    // 2. Normal Collapsed View Modes
    int style = g_settings.collapsedStyle;
    float peak = GetSystemAudioPeak();

    if (style == COLLAPSED_DYNAMIC)
    {
        if (peak > 0.005f || g_media.isPlaying || g_media.hasActiveSession)
            style = COLLAPSED_AUDIO;
        else
            style = COLLAPSED_TIME;
    }

    if (style == COLLAPSED_TIME)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t timeBuf[32];
        swprintf_s(timeBuf, L"%02d:%02d", st.wHour, st.wMinute);

        wchar_t dateBuf[32];
        const wchar_t *days[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
        swprintf_s(dateBuf, L"%s %d", days[st.wDayOfWeek % 7], st.wDay);

        if (!isVert)
        {
            HFONT clockFont = CreateClearTypeFont(11, FW_SEMIBOLD, L"Segoe UI");
            HGDIOBJ oldF = SelectObject(hdc, clockFont);
            SetTextColor(hdc, isDark ? RGB(230, 235, 245) : RGB(30, 36, 48));
            RECT timeRc = {14, 0, 70, height};
            DrawTextW(hdc, timeBuf, -1, &timeRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            int pillW = 32;
            int pillH = 3;
            int pillX = (width - pillW) / 2;
            int pillY = (height - pillH) / 2;
            COLORREF cLine = isDark ? RGB(75, 82, 100) : RGB(160, 170, 185);
            HBRUSH lBr = CreateSolidBrush(cLine);
            HPEN noP = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, lBr);
            SelectObject(hdc, noP);
            RoundRect(hdc, pillX, pillY, pillX + pillW, pillY + pillH, 3, 3);
            DeleteObject(lBr);
            DeleteObject(noP);

            HFONT dateFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe UI");
            SelectObject(hdc, dateFont);
            SetTextColor(hdc, isDark ? RGB(140, 150, 170) : RGB(100, 110, 125));
            RECT dateRc = {width - 75, 0, width - 14, height};
            DrawTextW(hdc, dateBuf, -1, &dateRc, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

            SelectObject(hdc, oldF);
            DeleteObject(clockFont);
            DeleteObject(dateFont);
        }
        else
        {
            HFONT clockFont = CreateClearTypeFont(10, FW_BOLD, L"Segoe UI");
            HGDIOBJ oldF = SelectObject(hdc, clockFont);
            SetTextColor(hdc, isDark ? RGB(230, 235, 245) : RGB(30, 36, 48));
            RECT topRc = {0, 12, width, 30};
            DrawTextW(hdc, timeBuf, -1, &topRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            int pillW = 3;
            int pillH = 26;
            int pillX = (width - pillW) / 2;
            int pillY = (height - pillH) / 2;
            COLORREF cLine = isDark ? RGB(75, 82, 100) : RGB(160, 170, 185);
            HBRUSH lBr = CreateSolidBrush(cLine);
            HPEN noP = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, lBr);
            SelectObject(hdc, noP);
            RoundRect(hdc, pillX, pillY, pillX + pillW, pillY + pillH, 3, 3);
            DeleteObject(lBr);
            DeleteObject(noP);

            SelectObject(hdc, oldF);
            DeleteObject(clockFont);
        }
    }
    else if (style == COLLAPSED_AUDIO)
    {
        ULONGLONG t = GetTickCount64();
        bool isPlaying = (peak > 0.005f || g_media.isPlaying);
        COLORREF accentCol = GetVisualizerAccentColor(g_settings.playerVisualizerColor);

        if (!isVert)
        {
            // Left: Album Artwork / Music Source Icon (Matches UI inspiration)
            if (g_settings.playerShowCollapsedArt && (g_media.hasActiveSession || isPlaying))
            {
                RenderCollapsedMusicThumbnail(hdc, 12, (height - 20) / 2, 20, isDark);
            }
            else
            {
                HFONT noteFont = CreateClearTypeFont(12, FW_NORMAL, L"Segoe MDL2 Assets");
                HGDIOBJ oldF = SelectObject(hdc, noteFont);
                SetTextColor(hdc, isPlaying ? accentCol : (isDark ? RGB(140, 150, 170) : RGB(100, 110, 125)));
                RECT noteRc = {14, 0, 34, height};
                DrawTextW(hdc, L"\xE8D6", -1, &noteRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                SelectObject(hdc, oldF);
                DeleteObject(noteFont);
            }

            // Center: Minimalist Grab Line
            int pillW = 28;
            int pillH = 3;
            int pillX = (width - pillW) / 2;
            int pillY = (height - pillH) / 2;
            COLORREF cLine = isDark ? RGB(75, 82, 100) : RGB(160, 170, 185);
            HBRUSH lBr = CreateSolidBrush(cLine);
            HPEN noP = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ oldB = SelectObject(hdc, lBr);
            HGDIOBJ oldP = SelectObject(hdc, noP);
            RoundRect(hdc, pillX, pillY, pillX + pillW, pillY + pillH, 3, 3);
            DeleteObject(lBr);

            // Right: Live Dancing Equalizer / Visualizer
            if (g_settings.playerShowVisualizer)
            {
                int numBars = 4;
                int barW = 3;
                int barGap = 3;
                int totalBarsW = numBars * barW + (numBars - 1) * barGap;
                int barStartX = width - 14 - totalBarsW;
                int maxBarH = 14;

                for (int b = 0; b < numBars; ++b)
                {
                    float wave = 0.5f + 0.5f * std::sin((t * 0.012f) + b * 1.4f);
                    float hFactor = isPlaying ? (peak * 0.5f + wave * 0.5f) : 0.25f;
                    int bH = std::max(4, static_cast<int>(maxBarH * hFactor));
                    int bX = barStartX + b * (barW + barGap);
                    int bY = (height - bH) / 2;

                    COLORREF barCol = isPlaying
                                          ? accentCol
                                          : (isDark ? RGB(75, 82, 100) : RGB(160, 170, 185));
                    HBRUSH bBr = CreateSolidBrush(barCol);
                    SelectObject(hdc, bBr);
                    RoundRect(hdc, bX, bY, bX + barW, bY + bH, 2, 2);
                    DeleteObject(bBr);
                }
            }
            SelectObject(hdc, oldB);
            SelectObject(hdc, oldP);
            DeleteObject(noP);
        }
        else
        {
            // Vertical dock collapsed audio
            if (g_settings.playerShowCollapsedArt && (g_media.hasActiveSession || isPlaying))
            {
                RenderCollapsedMusicThumbnail(hdc, (width - 20) / 2, 12, 20, isDark);
            }

            if (g_settings.playerShowVisualizer)
            {
                int numBars = 4;
                int barH = 3;
                int barGap = 3;
                int totalBarsH = numBars * barH + (numBars - 1) * barGap;
                int startY = height - 14 - totalBarsH;
                int maxBarW = 14;

                HPEN noP = CreatePen(PS_NULL, 0, 0);
                HGDIOBJ oldP = SelectObject(hdc, noP);

                for (int b = 0; b < numBars; ++b)
                {
                    float wave = 0.5f + 0.5f * std::sin((t * 0.012f) + b * 1.4f);
                    float wFactor = isPlaying ? (peak * 0.5f + wave * 0.5f) : 0.25f;
                    int bW = std::max(4, static_cast<int>(maxBarW * wFactor));
                    int bX = (width - bW) / 2;
                    int bY = startY + b * (barH + barGap);

                    COLORREF barCol = isPlaying ? accentCol : (isDark ? RGB(75, 82, 100) : RGB(160, 170, 185));
                    HBRUSH bBr = CreateSolidBrush(barCol);
                    HGDIOBJ oldB = SelectObject(hdc, bBr);
                    RoundRect(hdc, bX, bY, bX + bW, bY + barH, 2, 2);
                    SelectObject(hdc, oldB);
                    DeleteObject(bBr);
                }
                SelectObject(hdc, oldP);
                DeleteObject(noP);
            }
        }
    }
    else
    {
        int pillW = 38;
        int pillH = 4;
        int pillX = isVert ? (width - pillH) / 2 : (width - pillW) / 2;
        int pillY = isVert ? (height - pillW) / 2 : (height - pillH) / 2;

        COLORREF pillColor = isDark ? RGB(100, 108, 130) : RGB(45, 55, 75);
        COLORREF pillBorder = isDark ? RGB(75, 82, 100) : RGB(30, 40, 58);
        HBRUSH pillBrush = CreateSolidBrush(pillColor);
        HPEN pillPen = CreatePen(PS_SOLID, 1, pillBorder);
        SelectObject(hdc, pillBrush);
        SelectObject(hdc, pillPen);
        RoundRect(hdc, pillX, pillY, pillX + (isVert ? pillH : pillW), pillY + (isVert ? pillW : pillH), 4, 4);
        DeleteObject(pillBrush);
        DeleteObject(pillPen);
    }
}

// =======================================================================
// Actions Tab Panel Renderer (Tray Dock)
// Row 1: 3 Big Action Buttons (Record, Snip, Screenshot)
// Row 2: Quick Utility Row (Captures, Clipboard, Lock PC)
// =======================================================================
void RenderActionsTab(HDC hdc, int width, int height, bool isDark, int dockEdge)
{
    bool isLeft = (dockEdge == EDGE_LEFT);
    bool isRight = (dockEdge == EDGE_RIGHT);
    bool isVert = (isLeft || isRight);

    int startX = isLeft ? 48 : (isRight ? 16 : 20);
    int startY = isVert ? 24 : 36;
    int contentW = isVert ? (width - 64) : (width - 40);

    HFONT tileTitleFont = CreateClearTypeFont(12, FW_BOLD, L"Segoe UI");
    HFONT tileSubFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe UI");
    HFONT glyphFont = CreateClearTypeFont(18, FW_NORMAL, L"Segoe MDL2 Assets");
    HFONT miniIconFont = CreateClearTypeFont(13, FW_NORMAL, L"Segoe MDL2 Assets");

    COLORREF defaultTileBg = isDark ? RGB(26, 30, 40) : RGB(232, 238, 246);
    COLORREF defaultTileBorder = isDark ? RGB(45, 52, 68) : RGB(205, 215, 230);
    COLORREF textCol = isDark ? RGB(255, 255, 255) : RGB(18, 22, 32);
    COLORREF subCol = isDark ? RGB(140, 150, 170) : RGB(95, 105, 120);

    // =============================================================
    // ROW 1: 3 CORE BUTTONS (Record, Snip, Screenshot)
    // =============================================================
    bool isRec = g_recording.isRecording;

    if (!isVert)
    {
        // HORIZONTAL DOCK LAYOUT: 3 tiles side-by-side
        int gap = 10;
        int tileW = (contentW - gap * 2) / 3;
        int tileH = 68;

        g_actionRecBtnRect = {startX, startY, startX + tileW, startY + tileH};
        g_actionSnipBtnRect = {startX + tileW + gap, startY, startX + tileW * 2 + gap, startY + tileH};
        g_actionShotBtnRect = {startX + (tileW + gap) * 2, startY, startX + contentW, startY + tileH};

        // --- BUTTON 1: RECORD (Red Dot Icon) ---
        COLORREF recBg = isRec
                             ? (g_actionRecHovered ? RGB(70, 24, 28) : RGB(55, 18, 22))
                             : (g_actionRecHovered ? (isDark ? RGB(36, 42, 56) : RGB(220, 228, 240)) : defaultTileBg);
        COLORREF recBorder = isRec ? RGB(239, 68, 68) : defaultTileBorder;
        DrawRoundedRect(hdc, g_actionRecBtnRect, 12, recBg, recBorder, isRec ? 2 : 1);

        // Circular Icon Badge on Left
        int iconD = 32;
        int bX = g_actionRecBtnRect.left + 12;
        int bY = g_actionRecBtnRect.top + (tileH - iconD) / 2;
        RECT bRc = {bX, bY, bX + iconD, bY + iconD};
        COLORREF badgeBg = isRec ? RGB(239, 68, 68) : (isDark ? RGB(45, 50, 64) : RGB(214, 222, 234));
        DrawRoundedRect(hdc, bRc, 16, badgeBg, badgeBg, 0);

        HGDIOBJ oldF = SelectObject(hdc, glyphFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, isRec ? RGB(255, 255, 255) : RGB(239, 68, 68)); // Red dot icon
        DrawTextW(hdc, isRec ? L"\xE71A" : L"\xE7C8", -1, &bRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // Text
        SelectObject(hdc, tileTitleFont);
        SetTextColor(hdc, textCol);
        RECT t1Rc = {bRc.right + 10, g_actionRecBtnRect.top + 14, g_actionRecBtnRect.right - 8, g_actionRecBtnRect.top + 32};
        DrawTextW(hdc, isRec ? L"Stop" : L"Record", -1, &t1Rc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        SelectObject(hdc, tileSubFont);
        SetTextColor(hdc, isRec ? RGB(239, 68, 68) : subCol);
        std::wstring rSub = isRec ? GetRecordingDurationString() : L"Screen Video";
        RECT s1Rc = {bRc.right + 10, g_actionRecBtnRect.top + 34, g_actionRecBtnRect.right - 8, g_actionRecBtnRect.top + 50};
        DrawTextW(hdc, rSub.c_str(), -1, &s1Rc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        // --- BUTTON 2: SNIP (Scissor Icon) ---
        COLORREF snipBg = g_actionSnipHovered ? (isDark ? RGB(36, 42, 56) : RGB(220, 228, 240)) : defaultTileBg;
        DrawRoundedRect(hdc, g_actionSnipBtnRect, 12, snipBg, defaultTileBorder, 1);

        int b2X = g_actionSnipBtnRect.left + 12;
        int b2Y = g_actionSnipBtnRect.top + (tileH - iconD) / 2;
        RECT b2Rc = {b2X, b2Y, b2X + iconD, b2Y + iconD};
        COLORREF b2BadgeBg = isDark ? RGB(45, 50, 64) : RGB(214, 222, 234);
        DrawRoundedRect(hdc, b2Rc, 16, b2BadgeBg, b2BadgeBg, 0);

        SelectObject(hdc, glyphFont);
        SetTextColor(hdc, isDark ? RGB(56, 189, 248) : RGB(2, 132, 199)); // Scissor in Cyan/Blue
        DrawTextW(hdc, L"\xE8C6", -1, &b2Rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        SelectObject(hdc, tileTitleFont);
        SetTextColor(hdc, textCol);
        RECT t2Rc = {b2Rc.right + 10, g_actionSnipBtnRect.top + 14, g_actionSnipBtnRect.right - 8, g_actionSnipBtnRect.top + 32};
        DrawTextW(hdc, L"Snip", -1, &t2Rc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        SelectObject(hdc, tileSubFont);
        SetTextColor(hdc, subCol);
        RECT s2Rc = {b2Rc.right + 10, g_actionSnipBtnRect.top + 34, g_actionSnipBtnRect.right - 8, g_actionSnipBtnRect.top + 50};
        DrawTextW(hdc, L"Region Clip", -1, &s2Rc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        // --- BUTTON 3: SCREENSHOT (Window Icon) ---
        COLORREF shotBg = g_actionShotHovered ? (isDark ? RGB(36, 42, 56) : RGB(220, 228, 240)) : defaultTileBg;
        DrawRoundedRect(hdc, g_actionShotBtnRect, 12, shotBg, defaultTileBorder, 1);

        int b3X = g_actionShotBtnRect.left + 12;
        int b3Y = g_actionShotBtnRect.top + (tileH - iconD) / 2;
        RECT b3Rc = {b3X, b3Y, b3X + iconD, b3Y + iconD};
        DrawRoundedRect(hdc, b3Rc, 16, b2BadgeBg, b2BadgeBg, 0);

        SelectObject(hdc, glyphFont);
        SetTextColor(hdc, isDark ? RGB(74, 222, 128) : RGB(16, 185, 129)); // Screen icon in Emerald
        DrawTextW(hdc, L"\xE7F4", -1, &b3Rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        SelectObject(hdc, tileTitleFont);
        SetTextColor(hdc, textCol);
        RECT t3Rc = {b3Rc.right + 10, g_actionShotBtnRect.top + 14, g_actionShotBtnRect.right - 8, g_actionShotBtnRect.top + 32};
        DrawTextW(hdc, L"Screenshot", -1, &t3Rc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        SelectObject(hdc, tileSubFont);
        SetTextColor(hdc, subCol);
        RECT s3Rc = {b3Rc.right + 10, g_actionShotBtnRect.top + 34, g_actionShotBtnRect.right - 8, g_actionShotBtnRect.top + 50};
        DrawTextW(hdc, L"Full Desktop", -1, &s3Rc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    }
    else
    {
        // VERTICAL DOCK LAYOUT: 3 tiles vertically stacked
        int tileH = 50;
        int gap = 8;

        g_actionRecBtnRect = {startX, startY, startX + contentW, startY + tileH};
        g_actionSnipBtnRect = {startX, startY + tileH + gap, startX + contentW, startY + (tileH + gap) * 2 - gap};
        g_actionShotBtnRect = {startX, startY + (tileH + gap) * 2, startX + contentW, startY + (tileH + gap) * 3 - gap};

        auto renderVertTile = [&](const RECT &r, const wchar_t *glyph, const wchar_t *title, const wchar_t *sub, COLORREF iconCol, bool hov, bool active) {
            COLORREF bg = active
                              ? (hov ? RGB(70, 24, 28) : RGB(55, 18, 22))
                              : (hov ? (isDark ? RGB(36, 42, 56) : RGB(220, 228, 240)) : defaultTileBg);
            COLORREF border = active ? RGB(239, 68, 68) : defaultTileBorder;
            DrawRoundedRect(hdc, r, 10, bg, border, active ? 2 : 1);

            int iconD = 28;
            RECT bRc = {r.left + 10, r.top + (r.bottom - r.top - iconD) / 2, r.left + 10 + iconD, r.top + (r.bottom - r.top + iconD) / 2};
            COLORREF bCol = active ? RGB(239, 68, 68) : (isDark ? RGB(45, 50, 64) : RGB(214, 222, 234));
            DrawRoundedRect(hdc, bRc, 14, bCol, bCol, 0);

            SelectObject(hdc, glyphFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, active ? RGB(255, 255, 255) : iconCol);
            DrawTextW(hdc, glyph, -1, &bRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            SelectObject(hdc, tileTitleFont);
            SetTextColor(hdc, textCol);
            RECT tRc = {bRc.right + 10, r.top + 8, r.right - 8, r.top + 24};
            DrawTextW(hdc, title, -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            SelectObject(hdc, tileSubFont);
            SetTextColor(hdc, active ? RGB(239, 68, 68) : subCol);
            RECT sRc = {bRc.right + 10, r.top + 26, r.right - 8, r.top + 42};
            DrawTextW(hdc, sub, -1, &sRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        };

        std::wstring rSub = isRec ? GetRecordingDurationString() : L"Screen Video";
        renderVertTile(g_actionRecBtnRect, isRec ? L"\xE71A" : L"\xE7C8", isRec ? L"Stop" : L"Record", rSub.c_str(), RGB(239, 68, 68), g_actionRecHovered, isRec);
        renderVertTile(g_actionSnipBtnRect, L"\xE8C6", L"Snip", L"Region Clip", RGB(2, 132, 199), g_actionSnipHovered, false);
        renderVertTile(g_actionShotBtnRect, L"\xE7F4", L"Screenshot", L"Full Desktop", RGB(16, 185, 129), g_actionShotHovered, false);
    }

    // =============================================================
    // ROW 2: UTILITY ROW (Captures, Clipboard)
    // =============================================================
    int row2Y = isVert ? (g_actionShotBtnRect.bottom + 10) : (g_actionRecBtnRect.bottom + 12);
    int row2H = 34;
    int segW = (contentW - 8) / 2;

    g_actionFolderBtnRect = {startX, row2Y, startX + segW, row2Y + row2H};
    g_actionClipboardBtnRect = {startX + segW + 8, row2Y, startX + contentW, row2Y + row2H};
    g_actionLockBtnRect = {0, 0, 0, 0};

    auto drawUtilityPill = [&](const RECT &r, const wchar_t *icon, const wchar_t *label, bool hov) {
        COLORREF bg = hov ? (isDark ? RGB(40, 46, 60) : RGB(218, 226, 238)) : defaultTileBg;
        DrawRoundedRect(hdc, r, 8, bg, defaultTileBorder, 1);

        SelectObject(hdc, miniIconFont);
        SetTextColor(hdc, textCol);
        RECT iR = {r.left + 8, r.top, r.left + 26, r.bottom};
        DrawTextW(hdc, icon, -1, &iR, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        SelectObject(hdc, tileSubFont);
        RECT lR = {r.left + 28, r.top, r.right - 6, r.bottom};
        DrawTextW(hdc, label, -1, &lR, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    };

    drawUtilityPill(g_actionFolderBtnRect, L"\xE838", L"Captures", g_actionFolderHovered);
    drawUtilityPill(g_actionClipboardBtnRect, L"\xE77F", L"Clipboard", g_actionClipboardHovered);

    // =============================================================
    // RENDER FLOATING TOAST NOTIFICATION ON TOP
    // =============================================================
    RenderActionToast(hdc, width, height, isDark);

    DeleteObject(tileTitleFont);
    DeleteObject(tileSubFont);
    DeleteObject(glyphFont);
    DeleteObject(miniIconFont);
}

bool HandleActionsTabClick(HWND hwnd, POINT pt)
{
    // Row 1: The 3 Core Action Buttons
    if (PtInRect(&g_actionRecBtnRect, pt))
    {
        ToggleScreenRecording(hwnd);
        return true;
    }
    if (PtInRect(&g_actionSnipBtnRect, pt))
    {
        LaunchRegionSnippingTool();
        return true;
    }
    if (PtInRect(&g_actionShotBtnRect, pt))
    {
        CaptureInstantScreenshot(hwnd);
        return true;
    }

    // Row 2: Utilities
    if (PtInRect(&g_actionFolderBtnRect, pt))
    {
        std::wstring outDir = GetCapturesDirectory();
        ShellExecuteW(nullptr, L"open", outDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (PtInRect(&g_actionClipboardBtnRect, pt))
    {
        keybd_event(VK_LWIN, 0, 0, 0);
        keybd_event('V', 0, 0, 0);
        keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
        keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);
        return true;
    }
    return false;
}

bool HandleActionsTabMouseMove(HWND hwnd, POINT pt)
{
    bool needRedraw = false;

    auto updateHov = [&](bool &cur, const RECT &r) {
        bool nw = PtInRect(&r, pt);
        if (nw != cur)
        {
            cur = nw;
            needRedraw = true;
        }
    };

    updateHov(g_actionRecHovered, g_actionRecBtnRect);
    updateHov(g_actionSnipHovered, g_actionSnipBtnRect);
    updateHov(g_actionShotHovered, g_actionShotBtnRect);
    updateHov(g_actionFolderHovered, g_actionFolderBtnRect);
    updateHov(g_actionClipboardHovered, g_actionClipboardBtnRect);

    if (needRedraw && hwnd && IsWindow(hwnd))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return needRedraw;
}

int GetActionsTabDesiredWidth(int dockEdge)
{
    bool isVert = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);
    return isVert ? 260 : 490;
}

int GetActionsTabDesiredHeight(int dockEdge)
{
    bool isVert = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);
    return isVert ? 290 : 160;
}
