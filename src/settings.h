#pragma once
#include <windows.h>

void InitSettingsPath();
void LoadSettings();
void SaveSettings();
void OpenSettingsDialog(HWND parentHwnd);