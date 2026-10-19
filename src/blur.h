#pragma once
#include <windows.h>

enum class BackdropType { None = 1, Mica = 2, Acrylic = 3 };
void ApplyWindowBackdrop(HWND hwnd, BackdropType type);
