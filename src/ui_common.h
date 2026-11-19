#pragma once
#include <windows.h>
#include <string>
#include <algorithm>
#include <cstdint>

// ============================================================================
// WinTray Shared UI & Graphics Utilities
// ============================================================================

// Creates high-quality ClearType antialiased font
inline HFONT CreateClearTypeFont(int sizePx, int weight, const wchar_t *fontFamily = L"Segoe UI")
{
    return CreateFontW(-sizePx, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fontFamily);
}

// Draws a solid rounded rectangle with optional border
inline void DrawRoundedRect(HDC hdc, const RECT &r, int radius, COLORREF bg, COLORREF border, int borderWidth = 1)
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

// Draws a modern Fluent/iOS pill-shaped toggle switch
inline void DrawToggleSwitch(HDC hdc, const RECT &r, bool isOn, bool isDark, bool isHovered = false)
{
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int radius = h / 2;

    COLORREF trackBg, trackBorder, knobColor;
    if (isOn)
    {
        trackBg = isHovered ? RGB(5, 165, 115) : RGB(16, 185, 129);
        trackBorder = isHovered ? RGB(4, 140, 98) : RGB(13, 148, 103);
        knobColor = RGB(255, 255, 255);
    }
    else
    {
        trackBg = isDark ? (isHovered ? RGB(52, 60, 76) : RGB(42, 48, 62))
                         : (isHovered ? RGB(198, 208, 224) : RGB(214, 222, 234));
        trackBorder = isDark ? (isHovered ? RGB(80, 92, 115) : RGB(65, 75, 95))
                             : (isHovered ? RGB(165, 178, 198) : RGB(180, 192, 210));
        knobColor = isDark ? RGB(190, 200, 215) : RGB(255, 255, 255);
    }

    DrawRoundedRect(hdc, r, radius, trackBg, trackBorder, 1);

    int knobMargin = 2;
    int knobD = h - knobMargin * 2;
    int knobX = isOn ? (r.right - knobMargin - knobD) : (r.left + knobMargin);
    int knobY = r.top + knobMargin;

    RECT knobRc = {knobX, knobY, knobX + knobD, knobY + knobD};
    DrawRoundedRect(hdc, knobRc, knobD / 2, knobColor, knobColor, 0);
}

// Formats byte count into human-readable B, KB, MB, GB string
inline std::wstring FormatFileSize(ULONGLONG bytes)
{
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    if (bytes < 1024 * 1024) return std::to_wstring(bytes / 1024) + L" KB";
    if (bytes < 1024ULL * 1024 * 1024) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return buf;
    }
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

// Converts UTF-8 string to wide std::wstring
inline std::wstring Utf8ToWide(const std::string& str)
{
    if (str.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstr[0], sizeNeeded);
    return wstr;
}

// Converts wide std::wstring to UTF-8 std::string
inline std::string WideToUtf8(const std::wstring& wstr)
{
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &str[0], sizeNeeded, nullptr, nullptr);
    return str;
}

// Bulletproof path normalizer for Windows paths, VS Code & Web browser drops
inline std::wstring NormalizeWinPath(const std::wstring &rawPath)
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
