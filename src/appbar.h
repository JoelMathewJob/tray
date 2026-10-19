#pragma once
#include <windows.h>
#include <shellapi.h>

#define WM_APPBAR_CALLBACK (WM_USER + 101)

class AppBar {
public:
    static void Register(HWND hwnd, int barHeight);
    static void Unregister(HWND hwnd);
};
