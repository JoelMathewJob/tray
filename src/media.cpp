#include "media.h"
#include "state.h"
#include <shlobj.h>
#include <shellapi.h>
#include <wininet.h>
#include <wincrypt.h>
#include <algorithm>
#include <thread>
#include <vector>
#include <string>

#pragma comment(lib, "Crypt32.lib")

// -------------------------------------------------------------
// Helper: URL Decode (Converts %20 -> space for VS Code file:/// URIs)
// -------------------------------------------------------------
std::wstring UrlDecode(const std::wstring& str)
{
    std::wstring ret;
    ret.reserve(str.length());
    for (size_t i = 0; i < str.length(); ++i)
    {
        if (str[i] == L'%' && i + 2 < str.length())
        {
            wchar_t hex[3] = { str[i + 1], str[i + 2], 0 };
            wchar_t* endPtr = nullptr;
            wchar_t decoded = static_cast<wchar_t>(wcstol(hex, &endPtr, 16));
            if (endPtr == hex + 2)
            {
                ret += decoded;
                i += 2;
                continue;
            }
        }
        ret += str[i];
    }
    return ret;
}

// -------------------------------------------------------------
// Helper: Inspect File Header Bytes (Magic Bytes) for True Format
// -------------------------------------------------------------
std::wstring DetectExtensionFromBytes(const BYTE* data, size_t len, const std::wstring& fallbackExt)
{
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return L".png";
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return L".jpg";
    if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
        return L".gif";
    if (len >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
        return L".webp";
    if (len >= 2 && data[0] == 'B' && data[1] == 'M')
        return L".bmp";
    if (len >= 4 && data[0] == '%' && data[1] == 'P' && data[2] == 'D' && data[3] == 'F')
        return L".pdf";
    if (len >= 12 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p')
        return L".mp4";

    if (len >= 15)
    {
        std::string head(reinterpret_cast<const char*>(data), std::min<size_t>(len, 256));
        std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (head.find("<!doctype html") != std::string::npos || head.find("<html") != std::string::npos)
            return L".html";
    }

    return fallbackExt.empty() ? L".png" : fallbackExt;
}

// -------------------------------------------------------------
// Helper: Extract <img src="..."> from Browser HTML Clipboard Format
// -------------------------------------------------------------
std::wstring ExtractImgSrcFromHtml(const std::string& html)
{
    size_t imgPos = html.find("<img");
    if (imgPos == std::string::npos) imgPos = html.find("<IMG");
    if (imgPos == std::string::npos) return L"";

    size_t srcPos = html.find("src=", imgPos);
    if (srcPos == std::string::npos) srcPos = html.find("SRC=", imgPos);
    if (srcPos == std::string::npos) return L"";

    srcPos += 4;
    if (srcPos >= html.length()) return L"";

    char quote = html[srcPos];
    if (quote == '"' || quote == '\'')
    {
        srcPos++;
        size_t endPos = html.find(quote, srcPos);
        if (endPos != std::string::npos)
        {
            std::string url = html.substr(srcPos, endPos - srcPos);
            size_t amp = 0;
            while ((amp = url.find("&amp;", amp)) != std::string::npos)
            {
                url.replace(amp, 5, "&");
                amp += 1;
            }
            int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
            if (wlen > 1)
            {
                std::wstring wurl(wlen - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, &wurl[0], wlen);
                return wurl;
            }
        }
    }
    return L"";
}

// -------------------------------------------------------------
// Helper: Save DIB / Bitmap to Disk as a Valid 32-bit .bmp
// -------------------------------------------------------------
std::wstring SaveHBitmapToTemp(HBITMAP hBmp)
{
    if (!hBmp) return L"";

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring outDir = std::wstring(tempDir) + L"WinTray_Web";
    CreateDirectoryW(outDir.c_str(), nullptr);

    std::wstring outPath = outDir + L"\\Image_" + std::to_wstring(GetTickCount64()) + L".bmp";

    BITMAP bmp = {};
    if (!GetObject(hBmp, sizeof(BITMAP), &bmp)) return L"";

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = bmp.bmWidth;
    bih.biHeight = bmp.bmHeight;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    DWORD cbPixels = ((bmp.bmWidth * 32 + 31) / 32) * 4 * std::abs(bmp.bmHeight);
    std::vector<BYTE> pixels(cbPixels);

    HDC hdc = GetDC(nullptr);
    int copied = GetDIBits(hdc, hBmp, 0, std::abs(bmp.bmHeight), pixels.data(), reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);

    if (!copied) return L"";

    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + cbPixels;

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD written = 0;
    WriteFile(hFile, &bfh, sizeof(bfh), &written, nullptr);
    WriteFile(hFile, &bih, sizeof(bih), &written, nullptr);
    WriteFile(hFile, pixels.data(), cbPixels, &written, nullptr);
    CloseHandle(hFile);

    return outPath;
}

// -------------------------------------------------------------
// Background Worker: Asynchronous Web Media / Base64 Downloader
// -------------------------------------------------------------
void StartAsyncWebDownload(const std::wstring& url, HWND notifyHwnd)
{
    int itemId = AddPlaceholderItem(L"Downloading...");

    std::thread([url, itemId, notifyHwnd]() {
        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        std::wstring outDir = std::wstring(tempDir) + L"WinTray_Web";
        CreateDirectoryW(outDir.c_str(), nullptr);

        std::wstring finalPath = L"";
        std::wstring displayName = L"";
        bool success = false;

        // Case 1: Base64 Data URI (data:image/png;base64,...)
        if (url.rfind(L"data:", 0) == 0)
        {
            size_t commaPos = url.find(L',');
            if (commaPos != std::wstring::npos)
            {
                std::wstring b64Wide = url.substr(commaPos + 1);

                DWORD binLen = 0;
                if (CryptStringToBinaryW(b64Wide.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &binLen, nullptr, nullptr) && binLen > 0)
                {
                    std::vector<BYTE> buffer(binLen);
                    if (CryptStringToBinaryW(b64Wide.c_str(), 0, CRYPT_STRING_BASE64, buffer.data(), &binLen, nullptr, nullptr))
                    {
                        std::wstring ext = DetectExtensionFromBytes(buffer.data(), binLen, L".png");
                        displayName = L"WebImage_" + std::to_wstring(GetTickCount64() % 10000) + ext;
                        finalPath = outDir + L"\\" + displayName;

                        HANDLE hFile = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (hFile != INVALID_HANDLE_VALUE)
                        {
                            DWORD written = 0;
                            WriteFile(hFile, buffer.data(), binLen, &written, nullptr);
                            CloseHandle(hFile);
                            success = true;
                        }
                    }
                }
            }
        }
        // Case 2: Standard HTTP / HTTPS URL
        else if (url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0)
        {
            HINTERNET hInternet = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
            if (hInternet)
            {
                DWORD timeoutMs = 15000;
                InternetSetOptionW(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
                InternetSetOptionW(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

                HINTERNET hFile = InternetOpenUrlW(
                    hInternet, url.c_str(),
                    L"Accept: image/avif,image/webp,image/apng,image/*,video/*,*/*;q=0.8\r\n",
                    -1, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID, 0);

                if (hFile)
                {
                    std::vector<BYTE> fileBuffer;
                    BYTE chunk[16384];
                    DWORD bytesRead = 0;

                    while (InternetReadFile(hFile, chunk, sizeof(chunk), &bytesRead) && bytesRead > 0)
                    {
                        fileBuffer.insert(fileBuffer.end(), chunk, chunk + bytesRead);
                        if (fileBuffer.size() > 150 * 1024 * 1024) break;
                    }

                    InternetCloseHandle(hFile);

                    if (!fileBuffer.empty())
                    {
                        std::wstring baseName = L"media";
                        std::wstring urlExt = L"";
                        size_t slashPos = url.find_last_of(L"/\\");
                        if (slashPos != std::wstring::npos && slashPos + 1 < url.length())
                        {
                            std::wstring rawName = url.substr(slashPos + 1);
                            size_t qPos = rawName.find_first_of(L"?#&");
                            if (qPos != std::wstring::npos) rawName = rawName.substr(0, qPos);

                            for (auto& ch : rawName)
                            {
                                if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
                            }

                            size_t dotPos = rawName.find_last_of(L'.');
                            if (dotPos != std::wstring::npos)
                            {
                                baseName = rawName.substr(0, dotPos);
                                urlExt = rawName.substr(dotPos);
                            }
                            else if (!rawName.empty())
                            {
                                baseName = rawName;
                            }
                        }

                        if (baseName.empty()) baseName = L"web_item";
                        if (baseName.length() > 32) baseName = baseName.substr(0, 32);

                        std::wstring realExt = DetectExtensionFromBytes(fileBuffer.data(), fileBuffer.size(), urlExt);

                        if (realExt == L".html")
                        {
                            displayName = baseName + L".url";
                            finalPath = outDir + L"\\" + std::to_wstring(GetTickCount64() % 100000) + L"_" + displayName;
                            std::wstring shortcutContent = L"[InternetShortcut]\r\nURL=" + url + L"\r\n";

                            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, shortcutContent.c_str(), -1, nullptr, 0, nullptr, nullptr);
                            std::string utf8Content(utf8Len > 0 ? utf8Len - 1 : 0, '\0');
                            if (utf8Len > 1)
                            {
                                WideCharToMultiByte(CP_UTF8, 0, shortcutContent.c_str(), -1, &utf8Content[0], utf8Len, nullptr, nullptr);
                            }

                            HANDLE hOut = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (hOut != INVALID_HANDLE_VALUE)
                            {
                                DWORD written = 0;
                                WriteFile(hOut, utf8Content.data(), static_cast<DWORD>(utf8Content.size()), &written, nullptr);
                                CloseHandle(hOut);
                                success = true;
                            }
                        }
                        else
                        {
                            displayName = baseName + realExt;
                            finalPath = outDir + L"\\" + std::to_wstring(GetTickCount64() % 100000) + L"_" + displayName;

                            HANDLE hOut = CreateFileW(finalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (hOut != INVALID_HANDLE_VALUE)
                            {
                                DWORD written = 0;
                                WriteFile(hOut, fileBuffer.data(), static_cast<DWORD>(fileBuffer.size()), &written, nullptr);
                                CloseHandle(hOut);
                                success = true;
                            }
                        }
                    }
                }
                InternetCloseHandle(hInternet);
            }
        }

        auto* res = new DownloadResult{ itemId, success, finalPath, displayName };
        PostMessageW(notifyHwnd, WM_DOWNLOAD_COMPLETE, 0, reinterpret_cast<LPARAM>(res));
    }).detach();
}

// -------------------------------------------------------------
// Clipboard Paste Handler (Ctrl + V)
// -------------------------------------------------------------
void HandleClipboardPaste(HWND hwnd)
{
    if (!OpenClipboard(hwnd)) return;

    bool handledSync = false;

    if (IsClipboardFormatAvailable(CF_HDROP))
    {
        HANDLE hData = GetClipboardData(CF_HDROP);
        if (hData)
        {
            HDROP hDrop = static_cast<HDROP>(GlobalLock(hData));
            if (hDrop)
            {
                UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i)
                {
                    wchar_t filePath[MAX_PATH];
                    if (DragQueryFileW(hDrop, i, filePath, MAX_PATH))
                    {
                        AddFileToTray(filePath, false);
                        handledSync = true;
                    }
                }
                GlobalUnlock(hData);
            }
        }
    }
    else if (IsClipboardFormatAvailable(CF_BITMAP))
    {
        HBITMAP hBmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
        std::wstring saved = SaveHBitmapToTemp(hBmp);
        if (!saved.empty())
        {
            AddFileToTray(saved, false);
            handledSync = true;
        }
    }
    else if (IsClipboardFormatAvailable(CF_UNICODETEXT))
    {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData)
        {
            const wchar_t* pText = static_cast<const wchar_t*>(GlobalLock(hData));
            if (pText)
            {
                std::wstring str(pText);
                GlobalUnlock(hData);

                if (str.rfind(L"http://", 0) == 0 || str.rfind(L"https://", 0) == 0 || str.rfind(L"data:", 0) == 0)
                {
                    CloseClipboard();
                    TriggerExpand(hwnd);
                    StartAsyncWebDownload(str, hwnd);
                    return;
                }
                else if (GetFileAttributesW(str.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    AddFileToTray(str, false);
                    handledSync = true;
                }
            }
            else
            {
                GlobalUnlock(hData);
            }
        }
    }

    CloseClipboard();

    if (handledSync)
    {
        TriggerExpand(hwnd);
        CalculateTargetSize(g_isExpanded);
        SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

// -------------------------------------------------------------
// OLE IDropTarget for Instant Non-Blocking Drops
// -------------------------------------------------------------
class TrayDropTarget : public IDropTarget
{
    LONG m_cRef = 1;
    HWND m_hwnd;
    UINT m_cfHtml;

public:
    TrayDropTarget(HWND hwnd) : m_hwnd(hwnd)
    {
        m_cfHtml = RegisterClipboardFormatW(L"HTML Format");
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DragEnter(IDataObject *, DWORD, POINTL, DWORD *pdwEffect) override
    {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;
        TriggerExpand(m_hwnd);
        return S_OK;
    }

    STDMETHODIMP DragOver(DWORD, POINTL, DWORD *pdwEffect) override
    {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

    STDMETHODIMP DragLeave() override { return S_OK; }

    STDMETHODIMP Drop(IDataObject *pDataObj, DWORD, POINTL, DWORD *pdwEffect) override
    {
        if (!pdwEffect || !pDataObj) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;

        // 1. Check for real local files (CF_HDROP)
        FORMATETC fDrop = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm = {};
        if (pDataObj->GetData(&fDrop, &sm) == S_OK)
        {
            bool added = false;
            HDROP hDrop = static_cast<HDROP>(GlobalLock(sm.hGlobal));
            if (hDrop)
            {
                UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i)
                {
                    wchar_t filePath[MAX_PATH];
                    if (DragQueryFileW(hDrop, i, filePath, MAX_PATH))
                    {
                        AddFileToTray(filePath, false);
                        added = true;
                    }
                }
                GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);

            if (added)
            {
                CalculateTargetSize(g_isExpanded);
                SetTimer(m_hwnd, ANIM_TIMER_ID, 12, nullptr);
                InvalidateRect(m_hwnd, nullptr, TRUE);
                return S_OK;
            }
        }

        // 2. Check Browser "HTML Format" for true <img src="..."> media URL
        FORMATETC fHtml = { static_cast<CLIPFORMAT>(m_cfHtml), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        if (pDataObj->GetData(&fHtml, &sm) == S_OK)
        {
            const char* pHtml = static_cast<const char*>(GlobalLock(sm.hGlobal));
            if (pHtml)
            {
                std::string htmlStr(pHtml);
                GlobalUnlock(sm.hGlobal);
                ReleaseStgMedium(&sm);

                std::wstring imgSrc = ExtractImgSrcFromHtml(htmlStr);
                if (!imgSrc.empty())
                {
                    StartAsyncWebDownload(imgSrc, m_hwnd);
                    return S_OK;
                }
            }
            else
            {
                ReleaseStgMedium(&sm);
            }
        }

        // 3. Check for Raw Dropped Bitmap (CF_BITMAP)
        FORMATETC fBmp = { CF_BITMAP, nullptr, DVASPECT_CONTENT, -1, TYMED_GDI };
        if (pDataObj->GetData(&fBmp, &sm) == S_OK)
        {
            std::wstring saved = SaveHBitmapToTemp(sm.hBitmap);
            ReleaseStgMedium(&sm);
            if (!saved.empty())
            {
                AddFileToTray(saved, false);
                CalculateTargetSize(g_isExpanded);
                SetTimer(m_hwnd, ANIM_TIMER_ID, 12, nullptr);
                InvalidateRect(m_hwnd, nullptr, TRUE);
                return S_OK;
            }
        }

        // 4. Fallback to CF_UNICODETEXT (Direct image/video URLs, VS Code file:/// URIs)
        FORMATETC fUnicode = { CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        if (pDataObj->GetData(&fUnicode, &sm) == S_OK)
        {
            const wchar_t* pText = static_cast<const wchar_t*>(GlobalLock(sm.hGlobal));
            if (pText)
            {
                std::wstring data(pText);
                GlobalUnlock(sm.hGlobal);
                ReleaseStgMedium(&sm);

                if (data.rfind(L"file:///", 0) == 0)
                {
                    std::wstring localPath = UrlDecode(data.substr(8));
                    size_t endLine = localPath.find_first_of(L"\r\n");
                    if (endLine != std::wstring::npos) localPath = localPath.substr(0, endLine);
                    for (auto& ch : localPath) if (ch == L'/') ch = L'\\';

                    if (GetFileAttributesW(localPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        AddFileToTray(localPath, false);
                        CalculateTargetSize(g_isExpanded);
                        SetTimer(m_hwnd, ANIM_TIMER_ID, 12, nullptr);
                        InvalidateRect(m_hwnd, nullptr, TRUE);
                    }
                }
                else if (data.rfind(L"http://", 0) == 0 || data.rfind(L"https://", 0) == 0 || data.rfind(L"data:", 0) == 0)
                {
                    StartAsyncWebDownload(data, m_hwnd);
                }
                return S_OK;
            }
            ReleaseStgMedium(&sm);
        }

        return S_OK;
    }
};

static TrayDropTarget* g_dropTarget = nullptr;

void RegisterTrayDropTarget(HWND hwnd)
{
    if (!g_dropTarget)
        g_dropTarget = new TrayDropTarget(hwnd);
    RegisterDragDrop(hwnd, g_dropTarget);
}

void RevokeTrayDropTarget(HWND hwnd)
{
    RevokeDragDrop(hwnd);
}

// -------------------------------------------------------------
// OLE Drag-Out Classes
// -------------------------------------------------------------
class SimpleEnumFORMATETC : public IEnumFORMATETC
{
    LONG m_cRef = 1;
    std::vector<FORMATETC> m_formats;
    size_t m_index = 0;

public:
    SimpleEnumFORMATETC(const std::vector<FORMATETC> &formats) : m_formats(formats) {}
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC)
        {
            *ppv = static_cast<IEnumFORMATETC *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP Next(ULONG celt, FORMATETC *rgelt, ULONG *pceltFetched) override
    {
        ULONG fetched = 0;
        while (m_index < m_formats.size() && fetched < celt)
            rgelt[fetched++] = m_formats[m_index++];
        if (pceltFetched) *pceltFetched = fetched;
        return (fetched == celt) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG celt) override
    {
        m_index = std::min(m_index + celt, m_formats.size());
        return (m_index < m_formats.size()) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override { m_index = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumFORMATETC **ppenum) override
    {
        if (!ppenum) return E_POINTER;
        *ppenum = new SimpleEnumFORMATETC(m_formats);
        return S_OK;
    }
};

class SimpleDropSource : public IDropSource
{
    LONG m_cRef = 1;

public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropSource)
        {
            *ppv = static_cast<IDropSource *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override
    {
        if (fEscapePressed) return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

class MultiFileDataObject : public IDataObject
{
    LONG m_cRef = 1;
    std::vector<std::wstring> m_paths;
    UINT m_dropEffectFormat;
    DWORD m_effectToUse;

public:
    MultiFileDataObject(const std::vector<std::wstring> &paths, DWORD effect)
        : m_paths(paths), m_effectToUse(effect)
    {
        m_dropEffectFormat = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDataObject)
        {
            *ppv = static_cast<IDataObject *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP GetData(FORMATETC *pFormatetc, STGMEDIUM *pmedium) override
    {
        if (!pFormatetc || !pmedium) return E_POINTER;

        if (pFormatetc->cfFormat == CF_HDROP && (pFormatetc->tymed & TYMED_HGLOBAL))
        {
            SIZE_T strBytes = 0;
            for (const auto &p : m_paths)
                strBytes += (p.length() + 1) * sizeof(wchar_t);
            strBytes += sizeof(wchar_t);

            SIZE_T totalBytes = sizeof(DROPFILES) + strBytes;
            HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, totalBytes);
            if (!hGlobal) return E_OUTOFMEMORY;

            auto *df = static_cast<DROPFILES *>(GlobalLock(hGlobal));
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;

            wchar_t *dest = reinterpret_cast<wchar_t *>(reinterpret_cast<BYTE *>(df) + sizeof(DROPFILES));
            for (const auto &p : m_paths)
            {
                wcscpy_s(dest, p.length() + 1, p.c_str());
                dest += p.length() + 1;
            }
            *dest = L'\0';
            GlobalUnlock(hGlobal);

            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hGlobal;
            pmedium->pUnkForRelease = nullptr;
            return S_OK;
        }

        if (pFormatetc->cfFormat == m_dropEffectFormat && (pFormatetc->tymed & TYMED_HGLOBAL))
        {
            HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DWORD));
            if (!hGlobal) return E_OUTOFMEMORY;
            auto *pEffect = static_cast<DWORD *>(GlobalLock(hGlobal));
            *pEffect = m_effectToUse;
            GlobalUnlock(hGlobal);

            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hGlobal;
            pmedium->pUnkForRelease = nullptr;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    STDMETHODIMP QueryGetData(FORMATETC *pFormatetc) override
    {
        if (!pFormatetc) return E_POINTER;
        if (pFormatetc->cfFormat == CF_HDROP && (pFormatetc->tymed & TYMED_HGLOBAL)) return S_OK;
        if (pFormatetc->cfFormat == m_dropEffectFormat && (pFormatetc->tymed & TYMED_HGLOBAL)) return S_OK;
        return DV_E_FORMATETC;
    }
    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppenumFormatEtc) override
    {
        if (!ppenumFormatEtc) return E_POINTER;
        if (dwDirection == DATADIR_GET)
        {
            std::vector<FORMATETC> formats = {
                {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                {(CLIPFORMAT)m_dropEffectFormat, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
            *ppenumFormatEtc = new SimpleEnumFORMATETC(formats);
            return S_OK;
        }
        return E_NOTIMPL;
    }
    STDMETHODIMP GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC *, FORMATETC *) override { return E_NOTIMPL; }
    STDMETHODIMP SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return E_NOTIMPL; }
    STDMETHODIMP DUnadvise(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA **) override { return E_NOTIMPL; }
};

void StartDraggingSelectedOut(HWND hwnd, int clickedIdx)
{
    std::vector<std::wstring> pathsToDrag;
    std::vector<int> indicesToDrag;

    for (int i = 0; i < static_cast<int>(g_items.size()); ++i)
    {
        if (g_items[i].isSelected && !g_items[i].isLoading)
        {
            pathsToDrag.push_back(g_items[i].path);
            indicesToDrag.push_back(i);
        }
    }

    if (pathsToDrag.empty() && clickedIdx >= 0 && clickedIdx < static_cast<int>(g_items.size()) && !g_items[clickedIdx].isLoading)
    {
        pathsToDrag.push_back(g_items[clickedIdx].path);
        indicesToDrag.push_back(clickedIdx);
    }

    if (pathsToDrag.empty()) return;

    g_isSelfDragging = true;
    RevokeTrayDropTarget(hwnd);

    DWORD dropEffect = g_settings.moveFiles ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    auto *dataObj = new MultiFileDataObject(pathsToDrag, dropEffect);
    auto *dropSrc = new SimpleDropSource();

    DWORD dwEffectResult = 0;
    HRESULT hr = DoDragDrop(dataObj, dropSrc, dropEffect, &dwEffectResult);

    dataObj->Release();
    dropSrc->Release();

    if (hr == DRAGDROP_S_DROP && (dwEffectResult & (DROPEFFECT_COPY | DROPEFFECT_MOVE)))
    {
        if (g_settings.autoRemoveUnpinned || g_settings.moveFiles)
        {
            std::vector<std::wstring> toRemove;
            for (int idx : indicesToDrag)
            {
                if (idx >= 0 && idx < static_cast<int>(g_items.size()) && !g_items[idx].isPinned)
                    toRemove.push_back(g_items[idx].path);
            }

            for (const auto &removePath : toRemove)
            {
                auto it = std::find_if(g_items.begin(), g_items.end(), [&](const PinnedItem &item) { return item.path == removePath; });
                if (it != g_items.end())
                {
                    if (it->icon) DestroyIcon(it->icon);
                    g_items.erase(it);
                }
            }
            CalculateTargetSize(g_isExpanded);
            SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
        }
    }

    RegisterTrayDropTarget(hwnd);
    g_isSelfDragging = false;
    InvalidateRect(hwnd, nullptr, TRUE);
}

void CompressSelectedToZip(HWND hwnd)
{
    std::vector<std::wstring> selectedPaths;
    for (const auto &item : g_items)
    {
        if (item.isSelected && !item.isLoading)
            selectedPaths.push_back(item.path);
    }
    if (selectedPaths.empty()) return;

    std::wstring chosenZipPath = L"";
    IFileSaveDialog *pFileSave = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_IFileSaveDialog, reinterpret_cast<void **>(&pFileSave))))
    {
        COMDLG_FILTERSPEC fileTypes[] = {{L"ZIP Archive (*.zip)", L"*.zip"}};
        pFileSave->SetFileTypes(1, fileTypes);
        pFileSave->SetDefaultExtension(L"zip");
        pFileSave->SetFileName(L"tray_archive.zip");
        pFileSave->SetTitle(L"Save Compressed Tray Archive");

        if (SUCCEEDED(pFileSave->Show(hwnd)))
        {
            IShellItem *pResultItem = nullptr;
            if (SUCCEEDED(pFileSave->GetResult(&pResultItem)))
            {
                PWSTR pszFilePath = nullptr;
                if (SUCCEEDED(pResultItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath)))
                {
                    chosenZipPath = pszFilePath;
                    CoTaskMemFree(pszFilePath);
                }
                pResultItem->Release();
            }
        }
        pFileSave->Release();
    }

    if (chosenZipPath.empty()) return;

    std::wstring pathList = L"";
    for (size_t i = 0; i < selectedPaths.size(); ++i)
    {
        pathList += L"'" + selectedPaths[i] + L"'";
        if (i + 1 < selectedPaths.size()) pathList += L",";
    }

    std::wstring psCmd = L"/c powershell -NoProfile -Command \"Compress-Archive -Path " + pathList + L" -DestinationPath '" + chosenZipPath + L"' -Force\"";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = psCmd.c_str();
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei) && sei.hProcess)
    {
        WaitForSingleObject(sei.hProcess, 6000);
        CloseHandle(sei.hProcess);
        AddFileToTray(chosenZipPath, true);
    }

    CalculateTargetSize(g_isExpanded);
    SetTimer(hwnd, ANIM_TIMER_ID, 12, nullptr);
    InvalidateRect(hwnd, nullptr, TRUE);
}