#pragma once
#include "state.h"

int GetShareTabDesiredWidth(int dockEdge);
int GetShareTabDesiredHeight(int dockEdge);
void RenderShareTab(HDC hdc, int width, int height, bool isDark, int dockEdge);
bool HandleShareTabMouseMove(HWND hwnd, POINT pt);
bool HandleShareTabClick(HWND hwnd, POINT pt);
void SetShareActiveFile(const std::wstring& path);
