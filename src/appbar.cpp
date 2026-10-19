#include "appbar.h"

static APPBARDATA g_abd = {};

void AppBar::Register(HWND hwnd, int barHeight) {
    g_abd.cbSize = sizeof(APPBARDATA);
    g_abd.hWnd = hwnd;
    g_abd.uCallbackMessage = WM_APPBAR_CALLBACK;

    SHAppBarMessage(ABM_NEW, &g_abd);

    RECT rc = { 0, 0, GetSystemMetrics(SM_CXSCREEN), barHeight };
    g_abd.rc = rc;
    g_abd.uEdge = ABE_TOP;

    SHAppBarMessage(ABM_QUERYPOS, &g_abd);
    SHAppBarMessage(ABM_SETPOS, &g_abd);

    MoveWindow(hwnd, g_abd.rc.left, g_abd.rc.top,
               g_abd.rc.right - g_abd.rc.left,
               g_abd.rc.bottom - g_abd.rc.top, TRUE);
}

void AppBar::Unregister(HWND hwnd) {
    if (g_abd.hWnd == hwnd) {
        SHAppBarMessage(ABM_REMOVE, &g_abd);
        g_abd.hWnd = nullptr;
    }
}
