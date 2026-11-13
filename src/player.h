#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>

enum MediaSourceType
{
    MEDIA_SOURCE_UNKNOWN = 0,
    MEDIA_SOURCE_SPOTIFY = 1,
    MEDIA_SOURCE_YOUTUBE = 2,
    MEDIA_SOURCE_BROWSER = 3,
    MEDIA_SOURCE_APPLE = 4,
    MEDIA_SOURCE_GENERIC = 5
};

struct SystemMediaInfo
{
    bool hasActiveSession = false;
    bool isPlaying = false;
    std::wstring title = L"No Media Playing";
    std::wstring artist = L"WinTray Music";
    std::wstring album = L"";
    std::wstring appName = L"";
    MediaSourceType sourceType = MEDIA_SOURCE_GENERIC;

    LONGLONG positionMs = 0;
    LONGLONG durationMs = 0;

    Gdiplus::Bitmap *albumArt = nullptr;
    ULONGLONG lastUpdate = 0;
};

struct MediaFavorite
{
    std::wstring name;
    std::wstring subtitle;
    std::wstring url;
    const wchar_t *glyph = L"\xE8D6";
    COLORREF badgeColor = RGB(244, 114, 114);
    RECT rect = {};
    bool hovered = false;
};

extern SystemMediaInfo g_media;
extern std::vector<MediaFavorite> g_mediaFavorites;

// Hit Rectangles for Interactive Controls
extern RECT g_musicPrevBtnRect;
extern RECT g_musicPlayPauseBtnRect;
extern RECT g_musicNextBtnRect;
extern RECT g_musicProgressRect;
extern RECT g_musicVolSliderRect;
extern RECT g_musicAddFavBtnRect;

extern bool g_musicPrevHovered;
extern bool g_musicPlayPauseHovered;
extern bool g_musicNextHovered;
extern bool g_musicDraggingProgress;
extern bool g_musicDraggingVolume;
extern bool g_musicAddFavHovered;

void InitPlayerEngine();
void ShutdownPlayerEngine();
void UpdateSystemMediaState();

// Favorites Lifecycle
void InitDefaultFavorites();
void LoadMediaFavorites();
void SaveMediaFavorites();
void AddMediaFavorite(const std::wstring &name, const std::wstring &url, const std::wstring &sub = L"");
COLORREF GetVisualizerAccentColor(int colorIndex);

// Media Transport Commands
void MediaPreviousTrack();
void MediaPlayPauseTrack();
void MediaNextTrack();
void MediaSeekProgress(float ratio);

// Music Player Tab & Collapsed Thumbnail Renderers
void RenderMusicPlayerTab(HDC hdc, int width, int height, bool isDark, int dockEdge);
void RenderCollapsedMusicThumbnail(HDC hdc, int x, int y, int size, bool isDark);

// Hit Testing & Events
bool HandleMusicPlayerClick(HWND hwnd, POINT pt);
bool HandleMusicPlayerMouseMove(HWND hwnd, POINT pt);
void HandleMusicPlayerMouseUp(HWND hwnd);

int GetMusicPlayerDesiredWidth(int dockEdge);
int GetMusicPlayerDesiredHeight(int dockEdge);
