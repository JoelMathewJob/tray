#pragma once
#include <windows.h>
#include <oleidl.h>

void RegisterTrayDropTarget(HWND hwnd);
void RevokeTrayDropTarget(HWND hwnd);
void HandleClipboardPaste(HWND hwnd);
void StartDraggingSelectedOut(HWND hwnd, int clickedIdx);
void CompressSelectedToZip(HWND hwnd);