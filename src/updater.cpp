#include "updater.h"
#include "version_info.h"
#include <wininet.h>
#include <shellapi.h>
#include <string>

bool DownloadBinaryToDisk(const std::wstring &url, const std::wstring &destPath)
{
    HINTERNET hInternet = InternetOpenW(L"WinTrayUpdater", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet)
        return false;

    HINTERNET hFile = InternetOpenUrlW(
        hInternet, url.c_str(),
        L"User-Agent: WinTrayApp\r\n", -1,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);

    if (!hFile)
    {
        InternetCloseHandle(hInternet);
        return false;
    }

    HANDLE hOut = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE)
    {
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    char buffer[8192];
    DWORD bytesRead = 0;
    DWORD bytesWritten = 0;
    bool success = true;

    while (InternetReadFile(hFile, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0)
    {
        if (!WriteFile(hOut, buffer, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead)
        {
            success = false;
            break;
        }
    }

    CloseHandle(hOut);
    InternetCloseHandle(hFile);
    InternetCloseHandle(hInternet);

    if (!success)
    {
        DeleteFileW(destPath.c_str());
    }
    return success;
}

void ExecuteInPlaceUpdate(const std::wstring &newExePath)
{
    wchar_t currentExe[MAX_PATH];
    GetModuleFileNameW(nullptr, currentExe, MAX_PATH);

    std::wstring batchCmd =
        L"/c timeout /t 1 /nobreak > NUL & "
        L"move /y \"" + newExePath + L"\" \"" + currentExe + L"\" & "
        L"start \"\" \"" + currentExe + L"\"";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = batchCmd.c_str();
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei))
    {
        ExitProcess(0);
    }
}

void CheckForUpdates(HWND parentHwnd)
{
    HINTERNET hInternet = InternetOpenW(L"WinTrayUpdater", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet)
    {
        MessageBoxW(parentHwnd, L"Unable to access network to check for updates.", L"Update Check", MB_ICONWARNING);
        return;
    }

    std::wstring url = L"https://api.github.com/repos/" + GITHUB_REPO + L"/releases/latest";
    HINTERNET hFile = InternetOpenUrlW(
        hInternet, url.c_str(),
        L"User-Agent: WinTrayApp\r\n", -1,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);

    if (!hFile)
    {
        InternetCloseHandle(hInternet);
        MessageBoxW(parentHwnd, L"Could not query release server or repository does not exist.", L"Update Check", MB_ICONINFORMATION);
        return;
    }

    char buffer[4096] = {};
    DWORD bytesRead = 0;
    std::string response = "";
    while (InternetReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0)
    {
        buffer[bytesRead] = '\0';
        response += buffer;
    }
    InternetCloseHandle(hFile);
    InternetCloseHandle(hInternet);

    std::string tagKey = "\"tag_name\":\"";
    size_t tagPos = response.find(tagKey);
    if (tagPos == std::string::npos)
    {
        MessageBoxW(parentHwnd, L"No release information found.", L"Update Check", MB_ICONINFORMATION);
        return;
    }

    size_t startTag = tagPos + tagKey.length();
    size_t endTag = response.find("\"", startTag);
    std::string latestTag = response.substr(startTag, endTag - startTag);
    std::wstring wLatestTag(latestTag.begin(), latestTag.end());

    if (wLatestTag == CURRENT_VERSION)
    {
        std::wstring upToDateMsg = L"You are running the latest version of WinTray (" + CURRENT_VERSION + L").";
        MessageBoxW(parentHwnd, upToDateMsg.c_str(), L"WinTray is Up to Date", MB_ICONINFORMATION);
        return;
    }

    std::string downloadUrl = "";
    std::string urlKey = "\"browser_download_url\":\"";
    size_t searchPos = 0;
    while ((searchPos = response.find(urlKey, searchPos)) != std::string::npos)
    {
        size_t uStart = searchPos + urlKey.length();
        size_t uEnd = response.find("\"", uStart);
        std::string candidateUrl = response.substr(uStart, uEnd - uStart);
        if (candidateUrl.rfind("WinTray.exe") != std::string::npos)
        {
            downloadUrl = candidateUrl;
            break;
        }
        searchPos = uEnd;
    }

    if (downloadUrl.empty())
    {
        MessageBoxW(parentHwnd, L"New version found, but WinTray.exe was not attached to the release.", L"Update Error", MB_ICONWARNING);
        return;
    }

    std::wstring prompt = L"A new version (" + wLatestTag + L") is available!\nCurrent version: " + CURRENT_VERSION + L"\n\nWould you like to download and install it now?";
    if (MessageBoxW(parentHwnd, prompt.c_str(), L"Update Available", MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        return;
    }

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring tempExePath = std::wstring(tempDir) + L"WinTray_Update.exe";

    std::wstring wDownloadUrl(downloadUrl.begin(), downloadUrl.end());
    if (!DownloadBinaryToDisk(wDownloadUrl, tempExePath))
    {
        MessageBoxW(parentHwnd, L"Failed to download the update package.", L"Update Failed", MB_ICONERROR);
        return;
    }

    ExecuteInPlaceUpdate(tempExePath);
}