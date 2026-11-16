#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include "state.h"

int GetNotificationsTabDesiredWidth(int dockEdge);
int GetNotificationsTabDesiredHeight(int dockEdge);

void RenderNotificationsTab(HDC hdc, int width, int height, bool isDark, int dockEdge);
bool HandleNotificationsMouseMove(HWND hwnd, POINT pt);
bool HandleNotificationsClick(HWND hwnd, POINT pt);
void HandleNotificationsMouseUp(HWND hwnd);

void AddTransferNotification(const std::string& id, const std::wstring& senderName, const std::string& senderIp,
                             const std::wstring& fileName, ULONGLONG fileSize,
                             std::shared_ptr<std::atomic<int>> decision);
void AcceptNotification(const std::string& id);
void DeclineNotification(const std::string& id);
void ClearResolvedNotifications();
