#include "player.h"
#include "state.h"
#include "controls.h"
#include "actions.h"
#include "settings.h"
#include <shlobj.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;

SystemMediaInfo g_media = {};
std::vector<MediaFavorite> g_mediaFavorites;

RECT g_musicPrevBtnRect = {};
RECT g_musicPlayPauseBtnRect = {};
RECT g_musicNextBtnRect = {};
RECT g_musicProgressRect = {};
RECT g_musicVolSliderRect = {};
RECT g_musicAddFavBtnRect = {};

bool g_musicPrevHovered = false;
bool g_musicPlayPauseHovered = false;
bool g_musicNextHovered = false;
bool g_musicDraggingProgress = false;
bool g_musicDraggingVolume = false;
bool g_musicAddFavHovered = false;

static std::atomic<bool> s_workerRunning = false;
static std::thread s_workerThread;
static std::mutex s_mediaMutex;

COLORREF GetVisualizerAccentColor(int colorIndex)
{
    switch (colorIndex)
    {
    case 1: return RGB(56, 189, 248);  // Cyan Sky
    case 2: return RGB(34, 197, 94);   // Emerald Green
    case 3: return RGB(245, 158, 11);  // Neon Amber
    default: return RGB(244, 114, 114); // Coral Pink
    }
}

void InitDefaultFavorites()
{
    g_mediaFavorites.clear();
    g_mediaFavorites.push_back({L"Spotify", L"Music & Podcasts", L"spotify:", L"\xE8D6", RGB(30, 215, 96)});
    g_mediaFavorites.push_back({L"YouTube Music", L"music.youtube.com", L"https://music.youtube.com", L"\xE768", RGB(239, 68, 68)});
    g_mediaFavorites.push_back({L"Lofi Girl Radio", L"Beats to relax/study to", L"https://www.youtube.com/watch?v=jfKfPfyJRdk", L"\xE768", RGB(244, 114, 114)});
    g_mediaFavorites.push_back({L"YouTube", L"www.youtube.com", L"https://www.youtube.com", L"\xE774", RGB(220, 38, 38)});
    g_mediaFavorites.push_back({L"SoundCloud", L"soundcloud.com", L"https://soundcloud.com", L"\xE8D6", RGB(249, 115, 22)});
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

// Helper to reliably find the active media session across all apps (Chrome, Spotify, Edge, etc.)
static GlobalSystemMediaTransportControlsSession GetBestMediaSession(GlobalSystemMediaTransportControlsSessionManager mgr)
{
    if (!mgr) return nullptr;
    try
    {
        auto cur = mgr.GetCurrentSession();
        if (cur)
        {
            auto info = cur.GetPlaybackInfo();
            if (info && info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
            {
                return cur;
            }
        }

        auto sessions = mgr.GetSessions();
        if (sessions)
        {
            uint32_t count = sessions.Size();
            // 1. Prioritize any session that is currently Playing
            for (uint32_t i = 0; i < count; ++i)
            {
                try
                {
                    auto s = sessions.GetAt(i);
                    if (s)
                    {
                        auto info = s.GetPlaybackInfo();
                        if (info && info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                        {
                            return s;
                        }
                    }
                }
                catch (...) {}
            }

            // 2. Fall back to any session that has an open/paused track
            for (uint32_t i = 0; i < count; ++i)
            {
                try
                {
                    auto s = sessions.GetAt(i);
                    if (s)
                    {
                        auto info = s.GetPlaybackInfo();
                        if (info)
                        {
                            auto st = info.PlaybackStatus();
                            if (st != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed &&
                                st != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped)
                            {
                                return s;
                            }
                        }
                    }
                }
                catch (...) {}
            }
        }

        if (cur)
        {
            auto info = cur.GetPlaybackInfo();
            if (info && info.PlaybackStatus() != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed &&
                info.PlaybackStatus() != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped)
            {
                return cur;
            }
        }
    }
    catch (...) {}

    return nullptr;
}

// Background thread polling system media session every 250ms
static void MediaPollerThread()
{
    init_apartment(apartment_type::multi_threaded);

    int silentTicks = 0;

    while (s_workerRunning)
    {
        try
        {
            auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = GetBestMediaSession(mgr);
            float peak = GetSystemAudioPeak();

            if (session)
            {
                silentTicks = 0;
                auto info = session.GetPlaybackInfo();
                bool isPlaying = (info && info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                if (peak > 0.005f) isPlaying = true;

                std::wstring title = L"";
                std::wstring artist = L"";
                std::wstring album = L"";
                std::wstring appId = L"";
                Gdiplus::Bitmap *newBmp = nullptr;

                try
                {
                    appId = session.SourceAppUserModelId().c_str();
                }
                catch (...) {}

                try
                {
                    auto props = session.TryGetMediaPropertiesAsync().get();
                    if (props)
                    {
                        title = props.Title().c_str();
                        artist = props.Artist().c_str();
                        album = props.AlbumTitle().c_str();

                        if (props.Thumbnail())
                        {
                            try
                            {
                                auto stream = props.Thumbnail().OpenReadAsync().get();
                                uint64_t sz = stream.Size();
                                if (sz > 0 && sz < 10000000)
                                {
                                    std::vector<BYTE> buf(static_cast<size_t>(sz));
                                    DataReader reader(stream);
                                    reader.LoadAsync(static_cast<uint32_t>(sz)).get();
                                    reader.ReadBytes(buf);

                                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sz);
                                    if (hMem)
                                    {
                                        void *pMem = GlobalLock(hMem);
                                        if (pMem)
                                        {
                                            memcpy(pMem, buf.data(), static_cast<size_t>(sz));
                                            GlobalUnlock(hMem);
                                            IStream *pStream = nullptr;
                                            if (SUCCEEDED(CreateStreamOnHGlobal(hMem, TRUE, &pStream)))
                                            {
                                                newBmp = Gdiplus::Bitmap::FromStream(pStream);
                                                pStream->Release();
                                            }
                                        }
                                    }
                                }
                            }
                            catch (...) {}
                        }
                    }
                }
                catch (...) {}

                LONGLONG posMs = 0;
                LONGLONG durMs = 0;
                try
                {
                    auto timeline = session.GetTimelineProperties();
                    if (timeline)
                    {
                        posMs = timeline.Position().count() / 10000;
                        durMs = timeline.EndTime().count() / 10000;
                    }
                }
                catch (...) {}

                MediaSourceType src = MEDIA_SOURCE_GENERIC;
                std::wstring lowerApp = appId;
                std::transform(lowerApp.begin(), lowerApp.end(), lowerApp.begin(), ::towlower);
                if (lowerApp.find(L"spotify") != std::wstring::npos)
                    src = MEDIA_SOURCE_SPOTIFY;
                else if (lowerApp.find(L"chrome") != std::wstring::npos || lowerApp.find(L"edge") != std::wstring::npos ||
                         lowerApp.find(L"brave") != std::wstring::npos || lowerApp.find(L"firefox") != std::wstring::npos ||
                         lowerApp.find(L"opera") != std::wstring::npos)
                {
                    if (title.find(L"YouTube") != std::wstring::npos || lowerApp.find(L"youtube") != std::wstring::npos ||
                        artist.find(L"YouTube") != std::wstring::npos)
                        src = MEDIA_SOURCE_YOUTUBE;
                    else
                        src = MEDIA_SOURCE_BROWSER;
                }
                else if (lowerApp.find(L"apple") != std::wstring::npos)
                    src = MEDIA_SOURCE_APPLE;

                {
                    std::lock_guard<std::mutex> lock(s_mediaMutex);
                    g_media.hasActiveSession = true;
                    g_media.isPlaying = isPlaying;
                    if (!title.empty()) g_media.title = title;
                    if (!artist.empty()) g_media.artist = artist;
                    g_media.album = album;
                    g_media.appName = appId;
                    g_media.sourceType = src;
                    if (posMs > 0 || durMs > 0)
                    {
                        g_media.positionMs = posMs;
                        g_media.durationMs = durMs;
                    }
                    g_media.lastUpdate = GetTickCount64();

                    if (newBmp)
                    {
                        if (g_media.albumArt) delete g_media.albumArt;
                        g_media.albumArt = newBmp;
                    }
                }
            }
            else
            {
                // No GSMTC session detected
                if (peak > 0.005f)
                {
                    // Audio is actively playing through speakers!
                    silentTicks = 0;
                    std::lock_guard<std::mutex> lock(s_mediaMutex);
                    g_media.hasActiveSession = true;
                    g_media.isPlaying = true;
                    if (g_media.title.empty())
                    {
                        g_media.title = L"Playing Audio";
                        g_media.artist = L"System Output";
                    }
                    g_media.lastUpdate = GetTickCount64();
                }
                else
                {
                    // No session and silence
                    silentTicks++;
                    if (silentTicks >= 6) // 1.5 seconds of sustained silence & no media
                    {
                        std::lock_guard<std::mutex> lock(s_mediaMutex);
                        g_media.hasActiveSession = false;
                        g_media.isPlaying = false;
                        g_media.title = L"";
                        g_media.artist = L"";
                        g_media.album = L"";
                        g_media.positionMs = 0;
                        g_media.durationMs = 0;
                        if (g_media.albumArt)
                        {
                            delete g_media.albumArt;
                            g_media.albumArt = nullptr;
                        }
                    }
                }
            }
        }
        catch (...) {}

        // Sleep 250ms for responsive updates
        for (int i = 0; i < 5 && s_workerRunning; ++i)
        {
            Sleep(50);
        }
    }

    uninit_apartment();
}

void InitPlayerEngine()
{
    InitDefaultFavorites();
    if (!s_workerRunning)
    {
        s_workerRunning = true;
        s_workerThread = std::thread(MediaPollerThread);
    }
}

void ShutdownPlayerEngine()
{
    if (s_workerRunning)
    {
        s_workerRunning = false;
        if (s_workerThread.joinable())
        {
            s_workerThread.join();
        }
    }
    if (g_media.albumArt)
    {
        delete g_media.albumArt;
        g_media.albumArt = nullptr;
    }
}

void UpdateSystemMediaState()
{
    float peak = GetSystemAudioPeak();
    if (peak > 0.005f)
    {
        g_media.isPlaying = true;
    }
}

void MediaPreviousTrack()
{
    std::thread([]() {
        bool winRtSuccess = false;
        try
        {
            init_apartment(apartment_type::multi_threaded);
            auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = GetBestMediaSession(mgr);
            if (session)
            {
                session.TrySkipPreviousAsync().get();
                winRtSuccess = true;
            }
            uninit_apartment();
        }
        catch (...) {}

        if (!winRtSuccess)
        {
            keybd_event(VK_MEDIA_PREV_TRACK, 0, 0, 0);
            keybd_event(VK_MEDIA_PREV_TRACK, 0, KEYEVENTF_KEYUP, 0);
        }
    }).detach();
}

void MediaPlayPauseTrack()
{
    g_media.isPlaying = !g_media.isPlaying;
    if (g_mainHwnd && IsWindow(g_mainHwnd))
    {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }

    std::thread([]() {
        bool winRtSuccess = false;
        try
        {
            init_apartment(apartment_type::multi_threaded);
            auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = GetBestMediaSession(mgr);
            if (session)
            {
                session.TryTogglePlayPauseAsync().get();
                winRtSuccess = true;
            }
            uninit_apartment();
        }
        catch (...) {}

        if (!winRtSuccess)
        {
            keybd_event(VK_MEDIA_PLAY_PAUSE, 0, 0, 0);
            keybd_event(VK_MEDIA_PLAY_PAUSE, 0, KEYEVENTF_KEYUP, 0);
        }
    }).detach();
}

void MediaNextTrack()
{
    std::thread([]() {
        bool winRtSuccess = false;
        try
        {
            init_apartment(apartment_type::multi_threaded);
            auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = GetBestMediaSession(mgr);
            if (session)
            {
                session.TrySkipNextAsync().get();
                winRtSuccess = true;
            }
            uninit_apartment();
        }
        catch (...) {}

        if (!winRtSuccess)
        {
            keybd_event(VK_MEDIA_NEXT_TRACK, 0, 0, 0);
            keybd_event(VK_MEDIA_NEXT_TRACK, 0, KEYEVENTF_KEYUP, 0);
        }
    }).detach();
}

void MediaSeekProgress(float ratio)
{
    float clampedRatio = std::clamp(ratio, 0.0f, 1.0f);
    if (g_media.durationMs > 0)
    {
        g_media.positionMs = static_cast<LONGLONG>(g_media.durationMs * clampedRatio);
    }

    if (g_mainHwnd && IsWindow(g_mainHwnd))
    {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }

    std::thread([clampedRatio]() {
        try
        {
            init_apartment(apartment_type::multi_threaded);
            auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = GetBestMediaSession(mgr);
            if (session)
            {
                auto timeline = session.GetTimelineProperties();
                if (timeline)
                {
                    auto endVal = timeline.EndTime().count();
                    if (endVal > 0)
                    {
                        int64_t target100ns = static_cast<int64_t>(endVal * clampedRatio);
                        session.TryChangePlaybackPositionAsync(target100ns).get();
                    }
                }
            }
            uninit_apartment();
        }
        catch (...) {}
    }).detach();
}

static std::wstring FormatTrackTime(LONGLONG ms)
{
    int totalSec = std::max(0, static_cast<int>(ms / 1000));
    int m = totalSec / 60;
    int s = totalSec % 60;
    wchar_t buf[32];
    swprintf_s(buf, L"%d:%02d", m, s);
    return buf;
}

// Render sunset artwork fallback if no album art thumbnail is provided
static void DrawSunsetAlbumArt(HDC hdc, const RECT &r, int radius)
{
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::GraphicsPath path;
    int d = radius * 2;
    path.AddArc(r.left, r.top, d, d, 180, 90);
    path.AddArc(r.right - d, r.top, d, d, 270, 90);
    path.AddArc(r.right - d, r.bottom - d, d, d, 0, 90);
    path.AddArc(r.left, r.bottom - d, d, d, 90, 90);
    path.CloseFigure();

    g.SetClip(&path);

    // Warm Sunset Linear Gradient (Orange to Deep Purple)
    Gdiplus::Rect gRc(r.left, r.top, r.right - r.left, r.bottom - r.top);
    Gdiplus::LinearGradientBrush linBr(gRc, Gdiplus::Color(255, 255, 110, 60), Gdiplus::Color(255, 60, 20, 85), Gdiplus::LinearGradientModeVertical);
    g.FillRectangle(&linBr, gRc);

    // Sun Icon
    int w = r.right - r.left;
    int sunD = w * 4 / 10;
    int sunX = r.left + (w - sunD) / 2;
    int sunY = r.top + w * 3 / 10;
    Gdiplus::SolidBrush sunBr(Gdiplus::Color(255, 255, 220, 100));
    g.FillEllipse(&sunBr, sunX, sunY, sunD, sunD);

    // Ocean Waves at bottom
    int waveY = sunY + sunD / 2;
    Gdiplus::SolidBrush oceanBr(Gdiplus::Color(255, 40, 15, 60));
    g.FillRectangle(&oceanBr, r.left, waveY, w, r.bottom - waveY);

    Gdiplus::Pen wavePen(Gdiplus::Color(255, 255, 140, 90), 2.0f);
    for (int y = waveY + 4; y < r.bottom - 4; y += 5)
    {
        g.DrawLine(&wavePen, r.left + 8, y, r.right - 8, y);
    }

    g.ResetClip();
}

// Render the source badge (Spotify, YouTube, Browser, Apple, Note)
static void DrawSourceBadge(HDC hdc, int x, int y, int size, MediaSourceType src)
{
    COLORREF bgCol;
    const wchar_t *glyph;

    switch (src)
    {
    case MEDIA_SOURCE_SPOTIFY:
        bgCol = RGB(30, 215, 96); // Spotify Green
        glyph = L"\xE8D6";
        break;
    case MEDIA_SOURCE_YOUTUBE:
        bgCol = RGB(255, 0, 0); // YouTube Red
        glyph = L"\xE768";
        break;
    case MEDIA_SOURCE_APPLE:
        bgCol = RGB(250, 45, 72); // Apple Music Pink
        glyph = L"\xE8D6";
        break;
    default:
        bgCol = RGB(244, 114, 114); // Coral Music Note
        glyph = L"\xE8D6";
        break;
    }

    RECT bRc = {x, y, x + size, y + size};
    DrawRoundedRect(hdc, bRc, size / 2, bgCol, RGB(20, 20, 24), 2);

    HFONT bFont = CreateClearTypeFont(size * 6 / 10, FW_BOLD, L"Segoe MDL2 Assets");
    HGDIOBJ oldF = SelectObject(hdc, bFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, glyph, -1, &bRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    SelectObject(hdc, oldF);
    DeleteObject(bFont);
}

// =======================================================================
// Collapsed View Resting Thumbnail (Artwork + Source Badge)
// =======================================================================
void RenderCollapsedMusicThumbnail(HDC hdc, int x, int y, int size, bool isDark)
{
    RECT artRc = {x, y, x + size, y + size};

    if (g_media.albumArt)
    {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        Gdiplus::GraphicsPath path;
        int d = 6;
        path.AddArc(artRc.left, artRc.top, d, d, 180, 90);
        path.AddArc(artRc.right - d, artRc.top, d, d, 270, 90);
        path.AddArc(artRc.right - d, artRc.bottom - d, d, d, 0, 90);
        path.AddArc(artRc.left, artRc.bottom - d, d, d, 90, 90);
        path.CloseFigure();
        g.SetClip(&path);

        g.DrawImage(g_media.albumArt, artRc.left, artRc.top, size, size);
        g.ResetClip();
    }
    else
    {
        DrawSunsetAlbumArt(hdc, artRc, 4);
    }

    // Source Badge overlaid at bottom right of thumbnail
    int badgeD = 10;
    DrawSourceBadge(hdc, artRc.right - badgeD / 2, artRc.bottom - badgeD / 2, badgeD, g_media.sourceType);
}

// =======================================================================
// Expanded Music Player Tab Renderer (Active Player + Standby Favorites)
// =======================================================================
void RenderMusicPlayerTab(HDC hdc, int width, int height, bool isDark, int dockEdge)
{
    bool isLeft = (dockEdge == EDGE_LEFT);
    bool isRight = (dockEdge == EDGE_RIGHT);
    bool isVert = (isLeft || isRight);

    // Precise margins guaranteeing content never overlaps dock bezels or the navbar
    int startX = isLeft ? 46 : (isRight ? 12 : 24);
    int startY = isVert ? 22 : 36;
    int contentW = isVert ? (width - 46 - 12) : (width - 48);

    COLORREF textCol = isDark ? RGB(255, 255, 255) : RGB(18, 22, 32);
    COLORREF subCol = isDark ? RGB(160, 170, 190) : RGB(110, 120, 135);

    // ==============================================================
    // 1. STANDBY LAYOUT: NO ACTIVE MEDIA PLAYING + STREAM FAVORITES
    // ==============================================================
    float peak = GetSystemAudioPeak();
    bool isAudioActive = (g_media.hasActiveSession || peak > 0.005f || g_media.isPlaying);
    if (!isAudioActive)
    {
        if (g_mediaFavorites.empty())
        {
            InitDefaultFavorites();
        }

        if (!isVert)
        {
            // Horizontal Standby View
            int badgeD = 40;
            RECT bRc = {startX, startY, startX + badgeD, startY + badgeD};
            COLORREF badgeBg = isDark ? RGB(32, 36, 48) : RGB(225, 233, 246);
            COLORREF badgeBorder = isDark ? RGB(55, 62, 78) : RGB(200, 212, 230);
            DrawRoundedRect(hdc, bRc, badgeD / 2, badgeBg, badgeBorder, 1);

            HFONT vinylFont = CreateClearTypeFont(20, FW_NORMAL, L"Segoe MDL2 Assets");
            HGDIOBJ oldF = SelectObject(hdc, vinylFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, isDark ? RGB(244, 114, 114) : RGB(239, 68, 68));
            DrawTextW(hdc, L"\xE8D6", -1, &bRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            SelectObject(hdc, oldF);
            DeleteObject(vinylFont);

            int infoX = bRc.right + 14;
            int infoW = contentW - badgeD - 14 - 110;

            HFONT titleFont = CreateClearTypeFont(14, FW_BOLD, L"Segoe UI");
            oldF = SelectObject(hdc, titleFont);
            SetTextColor(hdc, textCol);
            RECT tRc = {infoX, startY, infoX + infoW, startY + 20};
            DrawTextW(hdc, L"No Audio Currently Playing", -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            HFONT subFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe UI");
            SelectObject(hdc, subFont);
            SetTextColor(hdc, subCol);
            RECT sRc = {infoX, startY + 20, infoX + infoW, startY + 38};
            DrawTextW(hdc, L"Play in Spotify, YouTube, or browser — or launch a favorite stream:", -1, &sRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            // "+ Settings" button
            int btnW = 96;
            int btnH = 26;
            g_musicAddFavBtnRect = {startX + contentW - btnW, startY + 4, startX + contentW, startY + 4 + btnH};
            COLORREF addBg = g_musicAddFavHovered ? (isDark ? RGB(45, 50, 68) : RGB(214, 222, 236)) : (isDark ? RGB(28, 32, 42) : RGB(232, 238, 248));
            COLORREF addBorder = isDark ? RGB(55, 62, 78) : RGB(200, 212, 230);
            DrawRoundedRect(hdc, g_musicAddFavBtnRect, 8, addBg, addBorder, 1);

            HFONT addFont = CreateClearTypeFont(10, FW_SEMIBOLD, L"Segoe UI");
            SelectObject(hdc, addFont);
            SetTextColor(hdc, isDark ? RGB(230, 235, 245) : RGB(24, 28, 38));
            DrawTextW(hdc, L"+ Settings", -1, &g_musicAddFavBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            SelectObject(hdc, oldF);
            DeleteObject(titleFont);
            DeleteObject(subFont);
            DeleteObject(addFont);

            // Horizontal Favorites Grid
            int gridY = startY + 48;
            int cardH = 58;
            int count = static_cast<int>(g_mediaFavorites.size());
            int gap = 8;
            int cardW = (contentW - (count - 1) * gap) / std::max(1, count);
            cardW = std::clamp(cardW, 70, 96);

            HFONT cardTitleFont = CreateClearTypeFont(11, FW_BOLD, L"Segoe UI");
            HFONT cardSubFont = CreateClearTypeFont(9, FW_NORMAL, L"Segoe UI");
            HFONT iconFont = CreateClearTypeFont(12, FW_NORMAL, L"Segoe MDL2 Assets");

            for (int i = 0; i < count; ++i)
            {
                int cX = startX + i * (cardW + gap);
                g_mediaFavorites[i].rect = {cX, gridY, cX + cardW, gridY + cardH};

                bool hov = g_mediaFavorites[i].hovered;
                COLORREF cBg = hov ? (isDark ? RGB(40, 46, 62) : RGB(216, 226, 240)) : (isDark ? RGB(26, 29, 38) : RGB(234, 240, 250));
                COLORREF cBorder = hov ? (isDark ? RGB(65, 75, 98) : RGB(170, 185, 210)) : (isDark ? RGB(45, 50, 65) : RGB(205, 215, 230));
                DrawRoundedRect(hdc, g_mediaFavorites[i].rect, 10, cBg, cBorder, 1);

                int bSz = 18;
                RECT iconRc = {cX + 8, gridY + 8, cX + 8 + bSz, gridY + 8 + bSz};
                DrawRoundedRect(hdc, iconRc, bSz / 2, g_mediaFavorites[i].badgeColor, g_mediaFavorites[i].badgeColor, 0);

                SelectObject(hdc, iconFont);
                SetTextColor(hdc, RGB(255, 255, 255));
                DrawTextW(hdc, g_mediaFavorites[i].glyph, -1, &iconRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                SelectObject(hdc, cardTitleFont);
                SetTextColor(hdc, textCol);
                RECT tRc = {cX + 8, gridY + 28, cX + cardW - 6, gridY + 42};
                DrawTextW(hdc, g_mediaFavorites[i].name.c_str(), -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

                SelectObject(hdc, cardSubFont);
                SetTextColor(hdc, subCol);
                RECT subRc = {cX + 8, gridY + 42, cX + cardW - 6, gridY + 54};
                DrawTextW(hdc, g_mediaFavorites[i].subtitle.c_str(), -1, &subRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            }

            DeleteObject(cardTitleFont);
            DeleteObject(cardSubFont);
            DeleteObject(iconFont);
        }
        else
        {
            // Vertical Standby View (Centered & Stacked for EDGE_LEFT / EDGE_RIGHT)
            int badgeD = 34;
            int bX = startX + (contentW - badgeD) / 2;
            RECT bRc = {bX, startY, bX + badgeD, startY + badgeD};
            COLORREF badgeBg = isDark ? RGB(32, 36, 48) : RGB(225, 233, 246);
            COLORREF badgeBorder = isDark ? RGB(55, 62, 78) : RGB(200, 212, 230);
            DrawRoundedRect(hdc, bRc, badgeD / 2, badgeBg, badgeBorder, 1);

            HFONT vinylFont = CreateClearTypeFont(18, FW_NORMAL, L"Segoe MDL2 Assets");
            HGDIOBJ oldF = SelectObject(hdc, vinylFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, isDark ? RGB(244, 114, 114) : RGB(239, 68, 68));
            DrawTextW(hdc, L"\xE8D6", -1, &bRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            DeleteObject(vinylFont);

            HFONT titleFont = CreateClearTypeFont(12, FW_BOLD, L"Segoe UI");
            SelectObject(hdc, titleFont);
            SetTextColor(hdc, textCol);
            RECT tRc = {startX, bRc.bottom + 4, startX + contentW, bRc.bottom + 20};
            DrawTextW(hdc, L"No Audio Playing", -1, &tRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            int btnW = 84;
            int btnH = 22;
            int btnX = startX + (contentW - btnW) / 2;
            g_musicAddFavBtnRect = {btnX, tRc.bottom + 4, btnX + btnW, tRc.bottom + 4 + btnH};
            COLORREF addBg = g_musicAddFavHovered ? (isDark ? RGB(45, 50, 68) : RGB(214, 222, 236)) : (isDark ? RGB(28, 32, 42) : RGB(232, 238, 248));
            COLORREF addBorder = isDark ? RGB(55, 62, 78) : RGB(200, 212, 230);
            DrawRoundedRect(hdc, g_musicAddFavBtnRect, 6, addBg, addBorder, 1);

            HFONT addFont = CreateClearTypeFont(9, FW_SEMIBOLD, L"Segoe UI");
            SelectObject(hdc, addFont);
            SetTextColor(hdc, isDark ? RGB(230, 235, 245) : RGB(24, 28, 38));
            DrawTextW(hdc, L"+ Settings", -1, &g_musicAddFavBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            DeleteObject(titleFont);
            DeleteObject(addFont);

            // Vertical Stacked Favorites
            int gridY = g_musicAddFavBtnRect.bottom + 8;
            int cardH = 36;
            int gap = 5;
            int count = std::min(4, static_cast<int>(g_mediaFavorites.size()));

            HFONT cardTitleFont = CreateClearTypeFont(10, FW_BOLD, L"Segoe UI");
            HFONT cardSubFont = CreateClearTypeFont(8, FW_NORMAL, L"Segoe UI");
            HFONT iconFont = CreateClearTypeFont(11, FW_NORMAL, L"Segoe MDL2 Assets");

            for (int i = 0; i < count; ++i)
            {
                int cY = gridY + i * (cardH + gap);
                g_mediaFavorites[i].rect = {startX, cY, startX + contentW, cY + cardH};

                bool hov = g_mediaFavorites[i].hovered;
                COLORREF cBg = hov ? (isDark ? RGB(40, 46, 62) : RGB(216, 226, 240)) : (isDark ? RGB(26, 29, 38) : RGB(234, 240, 250));
                COLORREF cBorder = hov ? (isDark ? RGB(65, 75, 98) : RGB(170, 185, 210)) : (isDark ? RGB(45, 50, 65) : RGB(205, 215, 230));
                DrawRoundedRect(hdc, g_mediaFavorites[i].rect, 8, cBg, cBorder, 1);

                int bSz = 18;
                RECT iconRc = {startX + 6, cY + 9, startX + 6 + bSz, cY + 9 + bSz};
                DrawRoundedRect(hdc, iconRc, bSz / 2, g_mediaFavorites[i].badgeColor, g_mediaFavorites[i].badgeColor, 0);

                SelectObject(hdc, iconFont);
                SetTextColor(hdc, RGB(255, 255, 255));
                DrawTextW(hdc, g_mediaFavorites[i].glyph, -1, &iconRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

                SelectObject(hdc, cardTitleFont);
                SetTextColor(hdc, textCol);
                RECT tRc = {startX + 30, cY + 3, startX + contentW - 6, cY + 18};
                DrawTextW(hdc, g_mediaFavorites[i].name.c_str(), -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

                SelectObject(hdc, cardSubFont);
                SetTextColor(hdc, subCol);
                RECT subRc = {startX + 30, cY + 18, startX + contentW - 6, cY + 32};
                DrawTextW(hdc, g_mediaFavorites[i].subtitle.c_str(), -1, &subRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            }

            DeleteObject(cardTitleFont);
            DeleteObject(cardSubFont);
            DeleteObject(iconFont);
        }
        return;
    }

    // ==============================================================
    // 2. ACTIVE MEDIA PLAYER LAYOUT (Song Playing)
    // ==============================================================
    // Compute extrapolated live position
    LONGLONG pos = g_media.positionMs;
    if (g_media.isPlaying && g_media.lastUpdate > 0 && !g_musicDraggingProgress)
    {
        ULONGLONG elapsed = GetTickCount64() - g_media.lastUpdate;
        pos += elapsed;
        if (g_media.durationMs > 0 && pos > g_media.durationMs)
            pos = g_media.durationMs;
    }
    LONGLONG dur = g_media.durationMs > 0 ? g_media.durationMs : 213000;
    if (g_media.durationMs == 0 && pos == 0) pos = 102000;
    float progressRatio = std::clamp(static_cast<float>(pos) / dur, 0.0f, 1.0f);

    std::wstring dispTitle = g_media.title.empty() ? L"Playing Audio" : g_media.title;
    std::wstring dispArtist = g_media.artist.empty() ? L"System Output" : g_media.artist;

    if (!isVert)
    {
        // ==========================================
        // HORIZONTAL DOCK LAYOUT (EDGE_TOP)
        // ==========================================
        HFONT titleFont = CreateClearTypeFont(15, FW_BOLD, L"Segoe UI");
        HFONT artistFont = CreateClearTypeFont(12, FW_NORMAL, L"Segoe UI");
        HFONT timeFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe UI");
        HFONT ctrlFont = CreateClearTypeFont(16, FW_NORMAL, L"Segoe MDL2 Assets");
        HFONT playFont = CreateClearTypeFont(20, FW_BOLD, L"Segoe MDL2 Assets");

        // 1. Album Artwork
        int artSize = 74;
        RECT artRc = {startX, startY + 2, startX + artSize, startY + 2 + artSize};

        if (g_media.albumArt)
        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath path;
            int d = 12 * 2;
            path.AddArc(artRc.left, artRc.top, d, d, 180, 90);
            path.AddArc(artRc.right - d, artRc.top, d, d, 270, 90);
            path.AddArc(artRc.right - d, artRc.bottom - d, d, d, 0, 90);
            path.AddArc(artRc.left, artRc.bottom - d, d, d, 90, 90);
            path.CloseFigure();
            g.SetClip(&path);
            g.DrawImage(g_media.albumArt, artRc.left, artRc.top, artSize, artSize);
            g.ResetClip();
        }
        else
        {
            DrawSunsetAlbumArt(hdc, artRc, 12);
        }

        int badgeD = 20;
        DrawSourceBadge(hdc, artRc.right - badgeD + 4, artRc.bottom - badgeD + 4, badgeD, g_media.sourceType);

        // 2. Track Title & Artist
        int infoX = artRc.right + 14;
        int ctrlW = 124;
        int infoW = contentW - artSize - 14 - ctrlW;

        HGDIOBJ oldF = SelectObject(hdc, titleFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, textCol);
        RECT tRc = {infoX, startY + 6, infoX + infoW, startY + 28};
        DrawTextW(hdc, dispTitle.c_str(), -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        SelectObject(hdc, artistFont);
        SetTextColor(hdc, subCol);
        RECT aRc = {infoX, startY + 28, infoX + infoW, startY + 48};
        DrawTextW(hdc, dispArtist.c_str(), -1, &aRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        // 3. Media Controls at top right
        int btnD = 32;
        int ctrlY = startY + 10;
        int ctrlRight = startX + contentW;

        g_musicNextBtnRect = {ctrlRight - btnD, ctrlY, ctrlRight, ctrlY + btnD};
        g_musicPlayPauseBtnRect = {g_musicNextBtnRect.left - btnD - 8, ctrlY - 2, g_musicNextBtnRect.left - 8, ctrlY + btnD + 2};
        g_musicPrevBtnRect = {g_musicPlayPauseBtnRect.left - btnD - 8, ctrlY, g_musicPlayPauseBtnRect.left - 8, ctrlY + btnD};

        SelectObject(hdc, ctrlFont);
        SetTextColor(hdc, g_musicPrevHovered ? (isDark ? RGB(255, 255, 255) : RGB(10, 15, 25)) : (isDark ? RGB(190, 200, 215) : RGB(80, 90, 110)));
        DrawTextW(hdc, L"\xE892", -1, &g_musicPrevBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        SelectObject(hdc, playFont);
        SetTextColor(hdc, g_musicPlayPauseHovered ? (isDark ? RGB(255, 255, 255) : RGB(10, 15, 25)) : (isDark ? RGB(235, 240, 250) : RGB(40, 50, 65)));
        DrawTextW(hdc, g_media.isPlaying ? L"\xE769" : L"\xE768", -1, &g_musicPlayPauseBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        SelectObject(hdc, ctrlFont);
        SetTextColor(hdc, g_musicNextHovered ? (isDark ? RGB(255, 255, 255) : RGB(10, 15, 25)) : (isDark ? RGB(190, 200, 215) : RGB(80, 90, 110)));
        DrawTextW(hdc, L"\xE893", -1, &g_musicNextBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // 4. Progress Scrubber
        int progY = artRc.bottom + 10;
        int timeW = 34;
        int progX = startX + timeW + 6;
        int progW = contentW - (timeW + 6) * 2;
        int progH = 12;

        g_musicProgressRect = {progX, progY, progX + progW, progY + progH};

        SelectObject(hdc, timeFont);
        SetTextColor(hdc, isDark ? RGB(160, 170, 190) : RGB(110, 120, 135));
        RECT leftTimeRc = {startX, progY - 1, startX + timeW, progY + progH + 1};
        DrawTextW(hdc, FormatTrackTime(pos).c_str(), -1, &leftTimeRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        int trackY = progY + progH / 2;
        int filledW = static_cast<int>(progW * progressRatio);

        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        COLORREF waveCol = GetVisualizerAccentColor(g_settings.playerVisualizerColor);
        Gdiplus::Pen wavePen(Gdiplus::Color(255, GetRValue(waveCol), GetGValue(waveCol), GetBValue(waveCol)), 2.5f);
        Gdiplus::Pen bgPen(isDark ? Gdiplus::Color(120, 255, 255, 255) : Gdiplus::Color(120, 0, 0, 0), 2.0f);

        Gdiplus::GraphicsPath wavePath;
        float step = 6.0f;
        for (float px = 0; px < filledW; px += step)
        {
            float wy = trackY + std::sin(px * 0.4f) * 2.5f;
            if (px == 0)
                wavePath.StartFigure();
            else
                wavePath.AddLine(progX + px - step, trackY + std::sin((px - step) * 0.4f) * 2.5f, progX + px, wy);
        }
        g.DrawPath(&wavePen, &wavePath);
        g.DrawLine(&bgPen, progX + filledW, trackY, progX + progW, trackY);

        int knobX = progX + filledW;
        Gdiplus::SolidBrush knobBr(Gdiplus::Color(255, 255, 255, 255));
        g.FillRectangle(&knobBr, knobX - 2, trackY - 6, 4, 12);

        LONGLONG remainMs = dur - pos;
        std::wstring remStr = L"-" + FormatTrackTime(remainMs);
        RECT rightTimeRc = {progX + progW + 6, progY - 1, startX + contentW, progY + progH + 1};
        DrawTextW(hdc, remStr.c_str(), -1, &rightTimeRc, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

        // 5. Volume Slider
        int volY = progY + progH + 6;
        int volW = 90;
        int volX = progX + (progW - volW) / 2;
        g_musicVolSliderRect = {volX, volY, volX + volW, volY + 8};

        HFONT spkFont = CreateClearTypeFont(11, FW_NORMAL, L"Segoe MDL2 Assets");
        SelectObject(hdc, spkFont);
        SetTextColor(hdc, isDark ? RGB(160, 170, 190) : RGB(110, 120, 135));
        RECT spkRc = {volX - 18, volY - 3, volX - 4, volY + 11};
        DrawTextW(hdc, L"\xE767", -1, &spkRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(hdc, timeFont);
        DeleteObject(spkFont);

        QuickControlsState &qc = GetQuickControlsState();
        int volFill = static_cast<int>(volW * qc.volume);
        Gdiplus::Pen vBgPen(isDark ? Gdiplus::Color(60, 255, 255, 255) : Gdiplus::Color(60, 0, 0, 0), 3.0f);
        Gdiplus::Pen vFillPen(isDark ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 20, 25, 35), 3.0f);
        g.DrawLine(&vBgPen, volX, volY + 4, volX + volW, volY + 4);
        g.DrawLine(&vFillPen, volX, volY + 4, volX + volFill, volY + 4);

        SelectObject(hdc, oldF);
        DeleteObject(titleFont);
        DeleteObject(artistFont);
        DeleteObject(timeFont);
        DeleteObject(ctrlFont);
        DeleteObject(playFont);
    }
    else
    {
        // ==========================================
        // VERTICAL DOCK LAYOUT (EDGE_LEFT / EDGE_RIGHT)
        // Clean Centered & Dynamic Stacked Architecture
        // ==========================================
        HFONT titleFont = CreateClearTypeFont(13, FW_BOLD, L"Segoe UI");
        HFONT artistFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe UI");
        HFONT timeFont = CreateClearTypeFont(9, FW_NORMAL, L"Segoe UI");
        HFONT ctrlFont = CreateClearTypeFont(15, FW_NORMAL, L"Segoe MDL2 Assets");
        HFONT playFont = CreateClearTypeFont(18, FW_BOLD, L"Segoe MDL2 Assets");

        // 1. Centered Album Art
        int artSize = 78;
        int artX = startX + (contentW - artSize) / 2;
        RECT artRc = {artX, startY + 4, artX + artSize, startY + 4 + artSize};

        if (g_media.albumArt)
        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath path;
            int d = 12 * 2;
            path.AddArc(artRc.left, artRc.top, d, d, 180, 90);
            path.AddArc(artRc.right - d, artRc.top, d, d, 270, 90);
            path.AddArc(artRc.right - d, artRc.bottom - d, d, d, 0, 90);
            path.AddArc(artRc.left, artRc.bottom - d, d, d, 90, 90);
            path.CloseFigure();
            g.SetClip(&path);
            g.DrawImage(g_media.albumArt, artRc.left, artRc.top, artSize, artSize);
            g.ResetClip();
        }
        else
        {
            DrawSunsetAlbumArt(hdc, artRc, 12);
        }

        int badgeD = 18;
        DrawSourceBadge(hdc, artRc.right - badgeD + 4, artRc.bottom - badgeD + 4, badgeD, g_media.sourceType);

        // 2. Centered Track Title & Artist
        int titleY = artRc.bottom + 6;
        HGDIOBJ oldF = SelectObject(hdc, titleFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, textCol);
        RECT tRc = {startX + 4, titleY, startX + contentW - 4, titleY + 18};
        DrawTextW(hdc, dispTitle.c_str(), -1, &tRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);

        SelectObject(hdc, artistFont);
        SetTextColor(hdc, subCol);
        RECT aRc = {startX + 4, titleY + 18, startX + contentW - 4, titleY + 32};
        DrawTextW(hdc, dispArtist.c_str(), -1, &aRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);

        // 3. Centered Media Controls
        int ctrlY = aRc.bottom + 12;
        int midX = startX + contentW / 2;
        g_musicPlayPauseBtnRect = {midX - 18, ctrlY, midX + 18, ctrlY + 34};
        g_musicPrevBtnRect = {g_musicPlayPauseBtnRect.left - 38, ctrlY + 2, g_musicPlayPauseBtnRect.left - 6, ctrlY + 32};
        g_musicNextBtnRect = {g_musicPlayPauseBtnRect.right + 6, ctrlY + 2, g_musicPlayPauseBtnRect.right + 38, ctrlY + 32};

        SelectObject(hdc, ctrlFont);
        SetTextColor(hdc, g_musicPrevHovered ? (isDark ? RGB(255, 255, 255) : RGB(10, 15, 25)) : (isDark ? RGB(190, 200, 215) : RGB(80, 90, 110)));
        DrawTextW(hdc, L"\xE892", -1, &g_musicPrevBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        SelectObject(hdc, playFont);
        SetTextColor(hdc, g_musicPlayPauseHovered ? (isDark ? RGB(255, 255, 255) : RGB(10, 15, 25)) : (isDark ? RGB(235, 240, 250) : RGB(40, 50, 65)));
        DrawTextW(hdc, g_media.isPlaying ? L"\xE769" : L"\xE768", -1, &g_musicPlayPauseBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        SelectObject(hdc, ctrlFont);
        SetTextColor(hdc, g_musicNextHovered ? (isDark ? RGB(255, 255, 255) : RGB(10, 15, 25)) : (isDark ? RGB(190, 200, 215) : RGB(80, 90, 110)));
        DrawTextW(hdc, L"\xE893", -1, &g_musicNextBtnRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // 4. Progress Scrubber
        int progY = g_musicPlayPauseBtnRect.bottom + 12;
        int timeW = 28;
        int progX = startX + timeW + 4;
        int progW = contentW - (timeW + 4) * 2;
        int progH = 10;

        g_musicProgressRect = {progX, progY, progX + progW, progY + progH};

        SelectObject(hdc, timeFont);
        SetTextColor(hdc, isDark ? RGB(160, 170, 190) : RGB(110, 120, 135));
        RECT leftTimeRc = {startX, progY - 1, startX + timeW, progY + progH + 1};
        DrawTextW(hdc, FormatTrackTime(pos).c_str(), -1, &leftTimeRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        int trackY = progY + progH / 2;
        int filledW = static_cast<int>(progW * progressRatio);

        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        COLORREF waveCol = GetVisualizerAccentColor(g_settings.playerVisualizerColor);
        Gdiplus::Pen wavePen(Gdiplus::Color(255, GetRValue(waveCol), GetGValue(waveCol), GetBValue(waveCol)), 2.0f);
        Gdiplus::Pen bgPen(isDark ? Gdiplus::Color(120, 255, 255, 255) : Gdiplus::Color(120, 0, 0, 0), 1.5f);

        Gdiplus::GraphicsPath wavePath;
        float step = 5.0f;
        for (float px = 0; px < filledW; px += step)
        {
            float wy = trackY + std::sin(px * 0.45f) * 2.0f;
            if (px == 0)
                wavePath.StartFigure();
            else
                wavePath.AddLine(progX + px - step, trackY + std::sin((px - step) * 0.45f) * 2.0f, progX + px, wy);
        }
        g.DrawPath(&wavePen, &wavePath);
        g.DrawLine(&bgPen, progX + filledW, trackY, progX + progW, trackY);

        int knobX = progX + filledW;
        Gdiplus::SolidBrush knobBr(Gdiplus::Color(255, 255, 255, 255));
        g.FillRectangle(&knobBr, knobX - 2, trackY - 5, 4, 10);

        LONGLONG remainMs = dur - pos;
        std::wstring remStr = L"-" + FormatTrackTime(remainMs);
        RECT rightTimeRc = {progX + progW + 4, progY - 1, startX + contentW, progY + progH + 1};
        DrawTextW(hdc, remStr.c_str(), -1, &rightTimeRc, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

        // 5. Centered Volume Slider in Vertical View
        int volY = progY + progH + 8;
        int volW = 76;
        int volX = startX + (contentW - volW) / 2;
        g_musicVolSliderRect = {volX, volY, volX + volW, volY + 6};

        HFONT spkFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe MDL2 Assets");
        SelectObject(hdc, spkFont);
        SetTextColor(hdc, isDark ? RGB(160, 170, 190) : RGB(110, 120, 135));
        RECT spkRc = {volX - 16, volY - 3, volX - 3, volY + 9};
        DrawTextW(hdc, L"\xE767", -1, &spkRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(hdc, timeFont);
        DeleteObject(spkFont);

        QuickControlsState &qc = GetQuickControlsState();
        int volFill = static_cast<int>(volW * qc.volume);
        Gdiplus::Pen vBgPen(isDark ? Gdiplus::Color(60, 255, 255, 255) : Gdiplus::Color(60, 0, 0, 0), 2.5f);
        Gdiplus::Pen vFillPen(isDark ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 20, 25, 35), 2.5f);
        g.DrawLine(&vBgPen, volX, volY + 3, volX + volW, volY + 3);
        g.DrawLine(&vFillPen, volX, volY + 3, volX + volFill, volY + 3);

        SelectObject(hdc, oldF);
        DeleteObject(titleFont);
        DeleteObject(artistFont);
        DeleteObject(timeFont);
        DeleteObject(ctrlFont);
        DeleteObject(playFont);
    }
}

bool HandleMusicPlayerClick(HWND hwnd, POINT pt)
{
    float peak = GetSystemAudioPeak();
    bool isAudioActive = (g_media.hasActiveSession || peak > 0.005f || g_media.isPlaying);
    if (!isAudioActive)
    {
        for (const auto &fav : g_mediaFavorites)
        {
            if (PtInRect(&fav.rect, pt))
            {
                ShellExecuteW(nullptr, L"open", fav.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
        }
        if (PtInRect(&g_musicAddFavBtnRect, pt))
        {
            OpenSettingsDialog(hwnd);
            return true;
        }
        return false;
    }

    if (PtInRect(&g_musicPrevBtnRect, pt))
    {
        MediaPreviousTrack();
        if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_musicPlayPauseBtnRect, pt))
    {
        MediaPlayPauseTrack();
        if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_musicNextBtnRect, pt))
    {
        MediaNextTrack();
        if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_musicProgressRect, pt))
    {
        g_musicDraggingProgress = true;
        int relX = pt.x - g_musicProgressRect.left;
        int w = g_musicProgressRect.right - g_musicProgressRect.left;
        if (w > 0)
        {
            float ratio = std::clamp(static_cast<float>(relX) / w, 0.0f, 1.0f);
            MediaSeekProgress(ratio);
        }
        if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_musicVolSliderRect, pt))
    {
        g_musicDraggingVolume = true;
        int relX = pt.x - g_musicVolSliderRect.left;
        int w = g_musicVolSliderRect.right - g_musicVolSliderRect.left;
        if (w > 0)
            SetSystemVolume(static_cast<float>(relX) / w);
        if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    return false;
}

bool HandleMusicPlayerMouseMove(HWND hwnd, POINT pt)
{
    bool needRedraw = false;

    float peak = GetSystemAudioPeak();
    bool isAudioActive = (g_media.hasActiveSession || peak > 0.005f || g_media.isPlaying);
    if (!isAudioActive)
    {
        for (auto &fav : g_mediaFavorites)
        {
            bool nw = PtInRect(&fav.rect, pt);
            if (nw != fav.hovered)
            {
                fav.hovered = nw;
                needRedraw = true;
            }
        }
        bool nwAdd = PtInRect(&g_musicAddFavBtnRect, pt);
        if (nwAdd != g_musicAddFavHovered)
        {
            g_musicAddFavHovered = nwAdd;
            needRedraw = true;
        }
    }
    else
    {
        auto updateHov = [&](bool &cur, const RECT &r) {
            bool nw = PtInRect(&r, pt);
            if (nw != cur)
            {
                cur = nw;
                needRedraw = true;
            }
        };

        updateHov(g_musicPrevHovered, g_musicPrevBtnRect);
        updateHov(g_musicPlayPauseHovered, g_musicPlayPauseBtnRect);
        updateHov(g_musicNextHovered, g_musicNextBtnRect);

        if (g_musicDraggingProgress)
        {
            int relX = pt.x - g_musicProgressRect.left;
            int w = g_musicProgressRect.right - g_musicProgressRect.left;
            if (w > 0)
            {
                float ratio = std::clamp(static_cast<float>(relX) / w, 0.0f, 1.0f);
                if (g_media.durationMs > 0)
                {
                    g_media.positionMs = static_cast<LONGLONG>(g_media.durationMs * ratio);
                }
                needRedraw = true;
            }
        }

        if (g_musicDraggingVolume)
        {
            int relX = pt.x - g_musicVolSliderRect.left;
            int w = g_musicVolSliderRect.right - g_musicVolSliderRect.left;
            if (w > 0)
            {
                SetSystemVolume(static_cast<float>(relX) / w);
                needRedraw = true;
            }
        }
    }

    if (needRedraw && hwnd && IsWindow(hwnd))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return needRedraw;
}

void HandleMusicPlayerMouseUp(HWND hwnd)
{
    if (g_musicDraggingProgress)
    {
        g_musicDraggingProgress = false;
        if (g_media.durationMs > 0)
        {
            float ratio = std::clamp(static_cast<float>(g_media.positionMs) / g_media.durationMs, 0.0f, 1.0f);
            MediaSeekProgress(ratio);
        }
        if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
    }
    g_musicDraggingVolume = false;
}

int GetMusicPlayerDesiredWidth(int dockEdge)
{
    bool isVert = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);
    return isVert ? 260 : 490;
}

int GetMusicPlayerDesiredHeight(int dockEdge)
{
    bool isVert = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);
    return isVert ? 280 : 156;
}
