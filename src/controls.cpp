#include "controls.h"
#include "state.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <wlanapi.h>
#include <shellapi.h>
#include <powrprof.h>
#include <wbemidl.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Radios.h>
#include <algorithm>
#include <cmath>
#include <thread>
#include <future>

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Radios;

static QuickControlsState g_state;
static IAudioEndpointVolume *g_pEndpointVolume = nullptr;
static bool g_comInitialized = false;

// Global interaction and hit rects
RECT g_tabFilesRect = {};
RECT g_tabControlsRect = {};
RECT g_tabActionsRect = {};
RECT g_tabMusicRect = {};
RECT g_settingsBtnRect = {};
RECT g_brightnessCapsuleRect = {};
RECT g_volumeCapsuleRect = {};
RECT g_tileRects[MAX_QUICK_TILES] = {};
int g_activeTileCount = 0;
RECT g_wifiArrowRect = {};
RECT g_btArrowRect = {};
RECT g_powerModeRects[3] = {};

enum QuickTileId
{
    TILE_WIFI = 0,
    TILE_BLUETOOTH = 1,
    TILE_AIRPLANE = 2,
    TILE_HOTSPOT = 3,
    TILE_NIGHTLIGHT = 4,
    TILE_FOCUS = 5,
    TILE_MUTE = 6,
    TILE_BATTERY = 7,
    TILE_LOCKPC = 8
};

static QuickTileId s_activeTileIds[MAX_QUICK_TILES] = {};

bool g_tabFilesHovered = false;
bool g_tabControlsHovered = false;
bool g_tabActionsHovered = false;
bool g_tabMusicHovered = false;
bool g_settingsHovered = false;
bool g_wifiArrowHovered = false;
bool g_btArrowHovered = false;
bool g_draggingBrightness = false;
bool g_draggingVolume = false;
int g_hoveredTile = -1;
int g_hoveredPowerMode = -1;

// Monitor brightness API (from dxva2.dll)
typedef BOOL(WINAPI *pfnGetNumberOfPhysicalMonitorsFromHMONITOR)(HMONITOR, LPDWORD);
typedef BOOL(WINAPI *pfnGetPhysicalMonitorsFromHMONITOR)(HMONITOR, DWORD, LPPHYSICAL_MONITOR);
typedef BOOL(WINAPI *pfnDestroyPhysicalMonitors)(DWORD, LPPHYSICAL_MONITOR);
typedef BOOL(WINAPI *pfnGetMonitorBrightness)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *pfnSetMonitorBrightness)(HANDLE, DWORD);

static HMODULE g_hDxva2 = nullptr;
static pfnGetNumberOfPhysicalMonitorsFromHMONITOR fnGetNumberOfPhysicalMonitors = nullptr;
static pfnGetPhysicalMonitorsFromHMONITOR fnGetPhysicalMonitors = nullptr;
static pfnDestroyPhysicalMonitors fnDestroyPhysicalMonitors = nullptr;
static pfnGetMonitorBrightness fnGetMonitorBrightness = nullptr;
static pfnSetMonitorBrightness fnSetMonitorBrightness = nullptr;

static void InitMonitorAPI()
{
    if (g_hDxva2)
        return;
    g_hDxva2 = LoadLibraryW(L"dxva2.dll");
    if (g_hDxva2)
    {
        fnGetNumberOfPhysicalMonitors = (pfnGetNumberOfPhysicalMonitorsFromHMONITOR)GetProcAddress(g_hDxva2, "GetNumberOfPhysicalMonitorsFromHMONITOR");
        fnGetPhysicalMonitors = (pfnGetPhysicalMonitorsFromHMONITOR)GetProcAddress(g_hDxva2, "GetPhysicalMonitorsFromHMONITOR");
        fnDestroyPhysicalMonitors = (pfnDestroyPhysicalMonitors)GetProcAddress(g_hDxva2, "DestroyPhysicalMonitors");
        fnGetMonitorBrightness = (pfnGetMonitorBrightness)GetProcAddress(g_hDxva2, "GetMonitorBrightness");
        fnSetMonitorBrightness = (pfnSetMonitorBrightness)GetProcAddress(g_hDxva2, "SetMonitorBrightness");
    }
}

// 1. Laptop WMI Internal Display Brightness (Windows Settings API)

static bool SetLaptopWmiBrightness(BYTE brightnessPercent)
{
    IWbemLocator *pLoc = nullptr;
    IWbemServices *pSvc = nullptr;
    IEnumWbemClassObject *pEnum = nullptr;
    IWbemClassObject *pObj = nullptr;
    IWbemClassObject *pInParams = nullptr;
    IWbemClassObject *pInInst = nullptr;
    IWbemClassObject *pOutParams = nullptr;
    IWbemClassObject *pClassMethod = nullptr;

    bool success = false;
    HRESULT hr;

    hr = CoCreateInstance(
        CLSID_WbemLocator,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        (LPVOID *)&pLoc);

    if (FAILED(hr))
        goto cleanup;

    {
        BSTR ns = SysAllocString(L"ROOT\\WMI");

        hr = pLoc->ConnectServer(
            ns, nullptr, nullptr, nullptr,
            0, nullptr, nullptr, &pSvc);

        SysFreeString(ns);
    }

    if (FAILED(hr))
        goto cleanup;

    CoSetProxyBlanket(
        pSvc,
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        nullptr,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE);

    {
        BSTR language = SysAllocString(L"WQL");
        BSTR query = SysAllocString(
            L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");

        hr = pSvc->ExecQuery(
            language,
            query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            &pEnum);

        SysFreeString(language);
        SysFreeString(query);
    }

    if (FAILED(hr))
        goto cleanup;

    {
        ULONG returned = 0;

        hr = pEnum->Next(
            WBEM_INFINITE,
            1,
            &pObj,
            &returned);

        if (FAILED(hr) || returned == 0)
            goto cleanup;
    }

    // Obtain the actual WMI monitor instance path.
    {
        VARIANT vtPath;
        VariantInit(&vtPath);

        hr = pObj->Get(
            L"__PATH",
            0,
            &vtPath,
            nullptr,
            nullptr);

        if (FAILED(hr) || vtPath.vt != VT_BSTR)
        {
            VariantClear(&vtPath);
            goto cleanup;
        }

        BSTR objectPath = SysAllocString(vtPath.bstrVal);
        VariantClear(&vtPath);

        hr = pObj->GetMethod(
            L"WmiSetBrightness",
            0,
            &pClassMethod,
            &pInParams);

        if (FAILED(hr) || !pInParams)
        {
            SysFreeString(objectPath);
            goto cleanup;
        }

        hr = pInParams->SpawnInstance(0, &pInInst);

        if (FAILED(hr))
        {
            SysFreeString(objectPath);
            goto cleanup;
        }

        VARIANT timeout;
        VariantInit(&timeout);
        timeout.vt = VT_UI4;
        timeout.ulVal = 0;

        VARIANT brightness;
        VariantInit(&brightness);
        brightness.vt = VT_UI1;
        brightness.bVal = brightnessPercent;

        pInInst->Put(L"Timeout", 0, &timeout, 0);
        pInInst->Put(L"Brightness", 0, &brightness, 0);

        SysFreeString(objectPath);
    }

    // Execute the method on the actual monitor instance.
    {
        VARIANT vtPath;
        VariantInit(&vtPath);

        hr = pObj->Get(
            L"__PATH",
            0,
            &vtPath,
            nullptr,
            nullptr);

        if (FAILED(hr) || vtPath.vt != VT_BSTR)
        {
            VariantClear(&vtPath);
            goto cleanup;
        }

        BSTR method = SysAllocString(L"WmiSetBrightness");

        hr = pSvc->ExecMethod(
            vtPath.bstrVal,
            method,
            0,
            nullptr,
            pInInst,
            &pOutParams,
            nullptr);

        SysFreeString(method);
        VariantClear(&vtPath);

        if (FAILED(hr) || !pOutParams)
            goto cleanup;

        VARIANT result;
        VariantInit(&result);

        hr = pOutParams->Get(
            L"ReturnValue",
            0,
            &result,
            nullptr,
            nullptr);

        if (SUCCEEDED(hr) &&
            result.vt == VT_UI4 &&
            result.ulVal == 0)
        {
            success = true;
        }

        VariantClear(&result);
    }

cleanup:

    if (pOutParams)
        pOutParams->Release();
    if (pInInst)
        pInInst->Release();
    if (pInParams)
        pInParams->Release();
    if (pClassMethod)
        pClassMethod->Release();
    if (pObj)
        pObj->Release();
    if (pEnum)
        pEnum->Release();
    if (pSvc)
        pSvc->Release();
    if (pLoc)
        pLoc->Release();

    return success;
}

static bool QueryLaptopWmiBrightness(float &outVal)
{
    IWbemLocator *pLoc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *)&pLoc)))
        return false;

    IWbemServices *pSvc = nullptr;
    BSTR bstrNamespace = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = pLoc->ConnectServer(
        bstrNamespace, // Namespace
        nullptr,       // User
        nullptr,       // Password
        nullptr,       // Locale
        0,             // Security flags (LONG)
        nullptr,       // Authority
        nullptr,       // Context
        &pSvc          // Output services
    );
    SysFreeString(bstrNamespace);

    if (FAILED(hr))
    {
        pLoc->Release();
        return false;
    }

    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    BSTR bstrWQL = SysAllocString(L"WQL");
    BSTR bstrQuery = SysAllocString(L"SELECT * FROM WmiMonitorBrightness WHERE Active=TRUE");
    IEnumWbemClassObject *pEnum = nullptr;
    hr = pSvc->ExecQuery(bstrWQL, bstrQuery, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
    SysFreeString(bstrWQL);
    SysFreeString(bstrQuery);

    bool success = false;
    if (SUCCEEDED(hr) && pEnum)
    {
        IWbemClassObject *pObj = nullptr;
        ULONG uReturned = 0;
        if (SUCCEEDED(pEnum->Next(WBEM_INFINITE, 1, &pObj, &uReturned)) && uReturned > 0)
        {
            VARIANT vtVal;
            VariantInit(&vtVal);
            if (SUCCEEDED(pObj->Get(L"CurrentBrightness", 0, &vtVal, nullptr, nullptr)))
            {
                outVal = static_cast<float>(vtVal.bVal) / 100.0f;
                success = true;
            }
            VariantClear(&vtVal);
            pObj->Release();
        }
        pEnum->Release();
    }

    pSvc->Release();
    pLoc->Release();
    return success;
}

// 2. Desktop DDC/CI External Monitor API
struct MonitorBrightnessParam
{
    bool set;
    float level;
    float queryResult;
    bool found;
};

static BOOL CALLBACK EnumPhysicalMonitorsProc(HMONITOR hMon, HDC hdc, LPRECT rc, LPARAM lParam)
{
    MonitorBrightnessParam *pParam = (MonitorBrightnessParam *)lParam;
    DWORD count = 0;
    if (fnGetNumberOfPhysicalMonitors && fnGetNumberOfPhysicalMonitors(hMon, &count) && count > 0)
    {
        PHYSICAL_MONITOR *pMonitors = new PHYSICAL_MONITOR[count];
        if (fnGetPhysicalMonitors && fnGetPhysicalMonitors(hMon, count, pMonitors))
        {
            for (DWORD i = 0; i < count; ++i)
            {

                if (pParam->set)
                {
                    DWORD minB = 0;
                    DWORD currentB = 0;
                    DWORD maxB = 100;

                    if (fnGetMonitorBrightness &&
                        fnGetMonitorBrightness(
                            pMonitors[i].hPhysicalMonitor,
                            &minB,
                            &currentB,
                            &maxB) &&
                        maxB > minB)
                    {
                        float normalized = std::clamp(
                            pParam->level, 0.0f, 1.0f);

                        DWORD val = minB + static_cast<DWORD>(
                                               std::round(normalized * (maxB - minB)));

                        if (fnSetMonitorBrightness)
                        {
                            fnSetMonitorBrightness(
                                pMonitors[i].hPhysicalMonitor,
                                val);
                        }
                    }
                }
                else
                {
                    DWORD minB = 0, curB = 100, maxB = 100;
                    if (fnGetMonitorBrightness && fnGetMonitorBrightness(pMonitors[i].hPhysicalMonitor, &minB, &curB, &maxB) && maxB > minB)
                    {
                        pParam->queryResult = static_cast<float>(curB - minB) / static_cast<float>(maxB - minB);
                        pParam->found = true;
                    }
                }
            }
            if (fnDestroyPhysicalMonitors)
                fnDestroyPhysicalMonitors(count, pMonitors);
        }
        delete[] pMonitors;
    }
    return TRUE;
}

static void QueryHardwareBrightness()
{
    float laptopVal = 1.0f;
    if (QueryLaptopWmiBrightness(laptopVal))
    {
        g_state.brightness = laptopVal;
        return;
    }

    InitMonitorAPI();
    MonitorBrightnessParam param = {false, 0.0f, 1.0f, false};
    EnumDisplayMonitors(nullptr, nullptr, EnumPhysicalMonitorsProc, (LPARAM)&param);
    if (param.found)
    {
        g_state.brightness = param.queryResult;
    }
}

static void SetHardwareBrightness(float level)
{
    level = std::clamp(level, 0.0f, 1.0f);

    BYTE percent = static_cast<BYTE>(
        std::lround(level * 100.0f));

    // First try the laptop's actual internal display.
    if (SetLaptopWmiBrightness(percent))
    {
        return;
    }

    // If WMI is unavailable, try external monitors.
    InitMonitorAPI();

    MonitorBrightnessParam param = {
        true, level, 0.0f, false};

    EnumDisplayMonitors(
        nullptr,
        nullptr,
        EnumPhysicalMonitorsProc,
        reinterpret_cast<LPARAM>(&param));
}
// 3. Full-Range Gamma Ramp & Night Light

static void ApplyDisplayBrightnessAndNightLight()
{
    float brightness = std::clamp(g_state.brightness, 0.0f, 1.0f);

    float factor = brightness;

    float redFactor = factor;
    float greenFactor = factor * (g_state.nightLightOn ? 0.88f : 1.0f);
    float blueFactor = factor * (g_state.nightLightOn ? 0.62f : 1.0f);

    WORD ramp[3][256];

    for (int i = 0; i < 256; ++i)
    {
        float input = static_cast<float>(i) / 255.0f;

        ramp[0][i] = static_cast<WORD>(
            std::clamp(input * redFactor, 0.0f, 1.0f) * 65535.0f);

        ramp[1][i] = static_cast<WORD>(
            std::clamp(input * greenFactor, 0.0f, 1.0f) * 65535.0f);

        ramp[2][i] = static_cast<WORD>(
            std::clamp(input * blueFactor, 0.0f, 1.0f) * 65535.0f);
    }

    HDC hdc = GetDC(nullptr);

    if (hdc)
    {
        SetDeviceGammaRamp(hdc, ramp);
        ReleaseDC(nullptr, hdc);
    }
}

// 4. Real Desktop Night Light (Windows 10/11 CloudStore)
static void SetDesktopNightLightNative(bool enable)
{
    HKEY hKey = nullptr;
    LPCWSTR subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\default$windows.data.bluelightreduction.bluelightreductionstate\\windows.data.bluelightreduction.bluelightreductionstate";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS)
    {
        DWORD type = REG_BINARY;
        BYTE buffer[256];
        DWORD size = sizeof(buffer);
        if (RegQueryValueExW(hKey, L"Data", nullptr, &type, buffer, &size) == ERROR_SUCCESS && size >= 20)
        {
            for (DWORD i = 0; i < size - 3; ++i)
            {
                if (buffer[i] == 0x2A && buffer[i + 1] == 0x2B && buffer[i + 2] == 0x0E)
                {
                    buffer[i + 3] = enable ? 0x13 : 0x15;
                    RegSetValueExW(hKey, L"Data", 0, REG_BINARY, buffer, size);
                    break;
                }
            }
        }
        RegCloseKey(hKey);
    }
}

static bool QueryDesktopNightLightNative()
{
    HKEY hKey = nullptr;
    LPCWSTR subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\default$windows.data.bluelightreduction.bluelightreductionstate\\windows.data.bluelightreduction.bluelightreductionstate";
    bool isOn = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD type = REG_BINARY;
        BYTE buffer[256];
        DWORD size = sizeof(buffer);
        if (RegQueryValueExW(hKey, L"Data", nullptr, &type, buffer, &size) == ERROR_SUCCESS && size >= 20)
        {
            for (DWORD i = 0; i < size - 3; ++i)
            {
                if (buffer[i] == 0x2A && buffer[i + 1] == 0x2B && buffer[i + 2] == 0x0E)
                {
                    isOn = (buffer[i + 3] == 0x13);
                    break;
                }
            }
        }
        RegCloseKey(hKey);
    }
    return isOn;
}

// 5. Native Windows Radio API (Bluetooth & Wi-Fi)
static void EnsureRadioConsent()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring escaped = exePath;
    std::replace(escaped.begin(), escaped.end(), L'\\', L'#');
    std::wstring regSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\radios\\NonPackaged\\" + escaped;

    HKEY hConsentKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, regSubKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hConsentKey, nullptr) == ERROR_SUCCESS)
    {
        const wchar_t val[] = L"Allow";
        RegSetValueExW(hConsentKey, L"Value", 0, REG_SZ, (const BYTE *)val, static_cast<DWORD>((wcslen(val) + 1) * sizeof(wchar_t)));
        RegCloseKey(hConsentKey);
    }
}

static void SetBluetoothRadioNative(bool enable)
{
    EnsureRadioConsent();
    std::thread([enable]()
                {
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto access = Radio::RequestAccessAsync().get();
            if (access == RadioAccessStatus::Allowed)
            {
                auto radios = Radio::GetRadiosAsync().get();
                for (auto const &radio : radios)
                {
                    if (radio.Kind() == RadioKind::Bluetooth)
                    {
                        RadioState targetState = enable ? RadioState::On : RadioState::Off;
                        radio.SetStateAsync(targetState).get();
                        break;
                    }
                }
            }
        }
        catch (...)
        {
        }
        winrt::uninit_apartment(); })
        .detach();
}

static bool QueryBluetoothRadioNative()
{
    EnsureRadioConsent();
    bool isOn = false;
    auto fut = std::async(std::launch::async, []()
                          {
        bool stateOn = false;
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto access = Radio::RequestAccessAsync().get();
            if (access == RadioAccessStatus::Allowed)
            {
                auto radios = Radio::GetRadiosAsync().get();
                for (auto const &radio : radios)
                {
                    if (radio.Kind() == RadioKind::Bluetooth)
                    {
                        stateOn = (radio.State() == RadioState::On);
                        break;
                    }
                }
            }
        }
        catch (...)
        {
        }
        winrt::uninit_apartment();
        return stateOn; });

    if (fut.wait_for(std::chrono::milliseconds(1200)) == std::future_status::ready)
    {
        isOn = fut.get();
    }
    return isOn;
}

// 6. Native WlanAPI (Wi-Fi)
static void SetHardwareWifiState(bool enable)
{
    HANDLE hClient = nullptr;
    DWORD curVer = 0;
    if (WlanOpenHandle(2, nullptr, &curVer, &hClient) == ERROR_SUCCESS)
    {
        PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
        if (WlanEnumInterfaces(hClient, nullptr, &pIfList) == ERROR_SUCCESS)
        {
            for (DWORD i = 0; i < pIfList->dwNumberOfItems; ++i)
            {
                WLAN_PHY_RADIO_STATE radioState = {};
                radioState.dwPhyIndex = 0;
                radioState.dot11SoftwareRadioState = enable ? dot11_radio_state_on : dot11_radio_state_off;
                WlanSetInterface(hClient, &pIfList->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_radio_state, sizeof(radioState), (PVOID)&radioState, nullptr);
            }
            WlanFreeMemory(pIfList);
        }
        WlanCloseHandle(hClient, nullptr);
    }

    EnsureRadioConsent();
    std::thread([enable]()
                {
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto access = Radio::RequestAccessAsync().get();
            if (access == RadioAccessStatus::Allowed)
            {
                auto radios = Radio::GetRadiosAsync().get();
                for (auto const &radio : radios)
                {
                    if (radio.Kind() == RadioKind::WiFi)
                    {
                        RadioState targetState = enable ? RadioState::On : RadioState::Off;
                        radio.SetStateAsync(targetState).get();
                        break;
                    }
                }
            }
        }
        catch (...)
        {
        }
        winrt::uninit_apartment(); })
        .detach();
}

static bool QueryHardwareWifiState()
{
    bool isOn = true;
    HANDLE hClient = nullptr;
    DWORD curVer = 0;
    if (WlanOpenHandle(2, nullptr, &curVer, &hClient) == ERROR_SUCCESS)
    {
        PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
        if (WlanEnumInterfaces(hClient, nullptr, &pIfList) == ERROR_SUCCESS)
        {
            if (pIfList->dwNumberOfItems > 0)
            {
                DWORD dataSize = 0;
                PVOID pData = nullptr;
                WLAN_OPCODE_VALUE_TYPE opType;
                if (WlanQueryInterface(hClient, &pIfList->InterfaceInfo[0].InterfaceGuid, wlan_intf_opcode_radio_state, nullptr, &dataSize, &pData, &opType) == ERROR_SUCCESS)
                {
                    PWLAN_RADIO_STATE pRadio = (PWLAN_RADIO_STATE)pData;
                    if (pRadio && pRadio->dwNumberOfPhys > 0)
                    {
                        isOn = (pRadio->PhyRadioState[0].dot11SoftwareRadioState == dot11_radio_state_on);
                    }
                    WlanFreeMemory(pData);
                }
            }
            WlanFreeMemory(pIfList);
        }
        WlanCloseHandle(hClient, nullptr);
    }
    return isOn;
}

void InitQuickControls()
{
    HRESULT hr = CoInitialize(nullptr);
    if (SUCCEEDED(hr))
        g_comInitialized = true;

    // Grant this executable radio access capability in registry
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring escaped = exePath;
    std::replace(escaped.begin(), escaped.end(), L'\\', L'#');
    std::wstring regSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\radios\\NonPackaged\\" + escaped;

    HKEY hConsentKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, regSubKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hConsentKey, nullptr) == ERROR_SUCCESS)
    {
        const wchar_t val[] = L"Allow";
        RegSetValueExW(hConsentKey, L"Value", 0, REG_SZ, (const BYTE *)val, sizeof(val));
        RegCloseKey(hConsentKey);
    }

    IMMDeviceEnumerator *pEnumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void **)&pEnumerator)))
    {
        IMMDevice *pDefaultDevice = nullptr;
        if (SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDefaultDevice)))
        {
            pDefaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, (void **)&g_pEndpointVolume);
            pDefaultDevice->Release();
        }
        pEnumerator->Release();
    }

    g_state.wifiOn = QueryHardwareWifiState();
    g_state.bluetoothOn = QueryBluetoothRadioNative();
    g_state.nightLightOn = QueryDesktopNightLightNative();

    QueryHardwareBrightness();
    UpdateQuickControlsState();
}

void ShutdownQuickControls()
{
    g_state.brightness = 1.0f;
    g_state.nightLightOn = false;
    ApplyDisplayBrightnessAndNightLight();

    if (g_pEndpointVolume)
    {
        g_pEndpointVolume->Release();
        g_pEndpointVolume = nullptr;
    }
    if (g_hDxva2)
    {
        FreeLibrary(g_hDxva2);
        g_hDxva2 = nullptr;
    }
    if (g_comInitialized)
    {
        CoUninitialize();
        g_comInitialized = false;
    }
}

void UpdateQuickControlsState()
{
    if (g_pEndpointVolume)
    {
        float vol = 1.0f;
        if (SUCCEEDED(g_pEndpointVolume->GetMasterVolumeLevelScalar(&vol)))
        {
            g_state.volume = vol;
        }
        BOOL muted = FALSE;
        if (SUCCEEDED(g_pEndpointVolume->GetMute(&muted)))
        {
            g_state.isMuted = (muted != FALSE);
        }
    }

    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps))
    {
        if (sps.BatteryLifePercent <= 100)
        {
            g_state.batteryPercent = sps.BatteryLifePercent;
            g_state.hasBattery = true;
        }
        else
        {
            g_state.batteryPercent = 100;
            g_state.hasBattery = false;
        }
        g_state.isPluggedIn = (sps.ACLineStatus == 1);
    }
}

QuickControlsState &GetQuickControlsState()
{
    return g_state;
}

void SetSystemVolume(float level)
{
    g_state.volume = std::clamp(level, 0.0f, 1.0f);
    if (g_pEndpointVolume)
    {
        g_pEndpointVolume->SetMasterVolumeLevelScalar(g_state.volume, nullptr);
        if (g_state.volume > 0.0f && g_state.isMuted)
        {
            g_pEndpointVolume->SetMute(FALSE, nullptr);
            g_state.isMuted = false;
        }
    }
}

void ToggleSystemMute()
{
    g_state.isMuted = !g_state.isMuted;
    if (g_pEndpointVolume)
    {
        g_pEndpointVolume->SetMute(g_state.isMuted ? TRUE : FALSE, nullptr);
    }
}

void SetSystemBrightness(float level)
{
    g_state.brightness = std::clamp(level, 0.0f, 1.0f);
    SetHardwareBrightness(g_state.brightness);
    ApplyDisplayBrightnessAndNightLight();
}

void ToggleWifi()
{
    g_state.wifiOn = !g_state.wifiOn;
    SetHardwareWifiState(g_state.wifiOn);
}

void ToggleBluetooth()
{
    g_state.bluetoothOn = !g_state.bluetoothOn;
    SetBluetoothRadioNative(g_state.bluetoothOn);
}

void ToggleNightLight()
{
    g_state.nightLightOn = !g_state.nightLightOn;
    SetDesktopNightLightNative(g_state.nightLightOn);
    ApplyDisplayBrightnessAndNightLight();
}

void ToggleFocus()
{
    g_state.focusOn = !g_state.focusOn;
}

void ToggleAirplaneMode()
{
    g_state.airplaneModeOn = !g_state.airplaneModeOn;
    if (g_state.airplaneModeOn)
    {
        g_state.wifiOn = false;
        g_state.bluetoothOn = false;
        SetHardwareWifiState(false);
        SetBluetoothRadioNative(false);
    }
    else
    {
        g_state.wifiOn = true;
        g_state.bluetoothOn = true;
        SetHardwareWifiState(true);
        SetBluetoothRadioNative(true);
    }
}

void ToggleMobileHotspot()
{
    g_state.hotspotOn = !g_state.hotspotOn;
    ShellExecuteW(nullptr, L"open", L"ms-settings:network-mobilehotspot", nullptr, nullptr, SW_SHOW);
}

void SetPowerMode(PowerMode mode)
{
    g_state.powerMode = mode;
    GUID schemeGuid;
    bool hasGuid = false;
    switch (mode)
    {
    case POWER_MODE_BATTERY:
        schemeGuid = GUID_MAX_POWER_SAVINGS;
        hasGuid = true;
        break;
    case POWER_MODE_BALANCED:
        schemeGuid = GUID_TYPICAL_POWER_SAVINGS;
        hasGuid = true;
        break;
    case POWER_MODE_PERFORMANCE:
        schemeGuid = GUID_MIN_POWER_SAVINGS;
        hasGuid = true;
        break;
    }
    if (hasGuid)
    {
        PowerSetActiveScheme(nullptr, &schemeGuid);
    }
}

// ClearType Font Creation (Smooth, high-contrast, subpixel rendering)
static HFONT CreateClearTypeFont(int sizePx, int weight, const wchar_t *fontFamily = L"Segoe UI")
{
    return CreateFontW(-sizePx, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fontFamily);
}

void RenderTopNavBar(HDC hdc, int width, int height, bool isDark, int activeTab, bool settingsHovered, int dockEdge)
{
    bool isLeft = (dockEdge == EDGE_LEFT);
    bool isRight = (dockEdge == EDGE_RIGHT);
    bool isVert = (isLeft || isRight);

    HFONT iconFont = CreateClearTypeFont(16, FW_NORMAL, L"Segoe MDL2 Assets");
    HGDIOBJ oldF = SelectObject(hdc, iconFont);
    SetBkMode(hdc, TRANSPARENT);

    COLORREF textInactive = isDark ? RGB(140, 148, 165) : RGB(140, 150, 168);
    COLORREF textHover = isDark ? RGB(220, 225, 240) : RGB(60, 70, 90);
    COLORREF textActive = isDark ? RGB(255, 255, 255) : RGB(15, 20, 30);

    if (isVert)
    {
        // ==========================================
        // VERTICAL TAB BAR (Along Left or Right Edge)
        // ==========================================
        int barX = isLeft ? 6 : (width - 34);
        int btnW = 28;
        int btnH = 26;
        int startY = 24;
        int gap = 8;

        int tabCount = g_settings.showMusicTab ? 4 : 3;
        g_tabFilesRect = {barX, startY, barX + btnW, startY + btnH};
        g_tabControlsRect = {barX, startY + btnH + gap, barX + btnW, startY + (btnH + gap) * 2};
        g_tabActionsRect = {barX, startY + (btnH + gap) * 2, barX + btnW, startY + (btnH + gap) * 3};
        if (g_settings.showMusicTab)
            g_tabMusicRect = {barX, startY + (btnH + gap) * 3, barX + btnW, startY + (btnH + gap) * 4};
        else
            g_tabMusicRect = {0, 0, 0, 0};

        int divY = startY + (btnH + gap) * tabCount + 6;
        int gearY = divY + 8;
        RECT gearRc = {barX, gearY, barX + btnW, gearY + btnH};
        g_settingsBtnRect = gearRc;

        // Files Tab
        bool filesActive = (activeTab == TAB_FILES);
        COLORREF filesCol = filesActive ? textActive : (g_tabFilesHovered ? textHover : textInactive);
        SetTextColor(hdc, filesCol);
        DrawTextW(hdc, L"\xE8B7", -1, &g_tabFilesRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        if (filesActive)
        {
            HBRUSH dotB = CreateSolidBrush(filesCol);
            HPEN noPen = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, dotB);
            SelectObject(hdc, noPen);
            int dotX = isLeft ? (g_tabFilesRect.right + 2) : (g_tabFilesRect.left - 5);
            int dotY = g_tabFilesRect.top + btnH / 2 - 2;
            RoundRect(hdc, dotX, dotY, dotX + 3, dotY + 4, 2, 2);
            DeleteObject(dotB);
            DeleteObject(noPen);
        }

        // Controls Tab
        bool controlsActive = (activeTab == TAB_CONTROLS);
        COLORREF controlsCol = controlsActive ? textActive : (g_tabControlsHovered ? textHover : textInactive);
        SetTextColor(hdc, controlsCol);
        DrawTextW(hdc, L"\xE790", -1, &g_tabControlsRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        if (controlsActive)
        {
            HBRUSH dotB = CreateSolidBrush(controlsCol);
            HPEN noPen = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, dotB);
            SelectObject(hdc, noPen);
            int dotX = isLeft ? (g_tabControlsRect.right + 2) : (g_tabControlsRect.left - 5);
            int dotY = g_tabControlsRect.top + btnH / 2 - 2;
            RoundRect(hdc, dotX, dotY, dotX + 3, dotY + 4, 2, 2);
            DeleteObject(dotB);
            DeleteObject(noPen);
        }

        // Actions Tab
        bool actionsActive = (activeTab == TAB_ACTIONS);
        COLORREF actionsCol = actionsActive ? textActive : (g_tabActionsHovered ? textHover : textInactive);
        SetTextColor(hdc, actionsCol);
        DrawTextW(hdc, L"\xE722", -1, &g_tabActionsRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        if (actionsActive)
        {
            HBRUSH dotB = CreateSolidBrush(actionsCol);
            HPEN noPen = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, dotB);
            SelectObject(hdc, noPen);
            int dotX = isLeft ? (g_tabActionsRect.right + 2) : (g_tabActionsRect.left - 5);
            int dotY = g_tabActionsRect.top + btnH / 2 - 2;
            RoundRect(hdc, dotX, dotY, dotX + 3, dotY + 4, 2, 2);
            DeleteObject(dotB);
            DeleteObject(noPen);
        }

        // Music Tab
        if (g_settings.showMusicTab)
        {
            bool musicActive = (activeTab == TAB_MUSIC);
            COLORREF musicCol = musicActive ? textActive : (g_tabMusicHovered ? textHover : textInactive);
            SetTextColor(hdc, musicCol);
            DrawTextW(hdc, L"\xE8D6", -1, &g_tabMusicRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            if (musicActive)
            {
                HBRUSH dotB = CreateSolidBrush(musicCol);
                HPEN noPen = CreatePen(PS_NULL, 0, 0);
                SelectObject(hdc, dotB);
                SelectObject(hdc, noPen);
                int dotX = isLeft ? (g_tabMusicRect.right + 2) : (g_tabMusicRect.left - 5);
                int dotY = g_tabMusicRect.top + btnH / 2 - 2;
                RoundRect(hdc, dotX, dotY, dotX + 3, dotY + 4, 2, 2);
                DeleteObject(dotB);
                DeleteObject(noPen);
            }
        }

        // Horizontal Divider in vertical bar
        HPEN divPen = CreatePen(PS_SOLID, 1, isDark ? RGB(65, 70, 85) : RGB(185, 195, 210));
        SelectObject(hdc, divPen);
        MoveToEx(hdc, barX + 3, divY, nullptr);
        LineTo(hdc, barX + btnW - 3, divY);
        DeleteObject(divPen);

        // Settings Gear
        COLORREF gearCol = settingsHovered ? textActive : textInactive;
        SetTextColor(hdc, gearCol);
        DrawTextW(hdc, L"\xE713", -1, &gearRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    else
    {
        // ==========================================
        // HORIZONTAL TAB BAR (Along Top Edge)
        // ==========================================
        int rowY = 5;
        int btnW = 28;
        int btnH = 22;
        int gap = 6;
        int divW = 12;

        int tabCount = g_settings.showMusicTab ? 4 : 3;
        int totalBarW = (btnW + gap) * tabCount + divW + gap + btnW;
        int startX = (width - totalBarW) / 2;

        g_tabFilesRect = {startX, rowY, startX + btnW, rowY + btnH};
        g_tabControlsRect = {startX + btnW + gap, rowY, startX + btnW + gap + btnW, rowY + btnH};
        g_tabActionsRect = {startX + (btnW + gap) * 2, rowY, startX + (btnW + gap) * 2 + btnW, rowY + btnH};
        if (g_settings.showMusicTab)
            g_tabMusicRect = {startX + (btnW + gap) * 3, rowY, startX + (btnW + gap) * 3 + btnW, rowY + btnH};
        else
            g_tabMusicRect = {0, 0, 0, 0};

        // Files Tab
        bool filesActive = (activeTab == TAB_FILES);
        COLORREF filesCol = filesActive ? textActive : (g_tabFilesHovered ? textHover : textInactive);
        SetTextColor(hdc, filesCol);
        DrawTextW(hdc, L"\xE8B7", -1, &g_tabFilesRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        if (filesActive)
        {
            HBRUSH dotB = CreateSolidBrush(filesCol);
            HPEN noPen = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, dotB);
            SelectObject(hdc, noPen);
            int dotX = g_tabFilesRect.left + btnW / 2;
            int dotY = g_tabFilesRect.bottom + 1;
            RoundRect(hdc, dotX - 2, dotY, dotX + 2, dotY + 4, 2, 2);
            DeleteObject(dotB);
            DeleteObject(noPen);
        }

        // Controls Tab
        bool controlsActive = (activeTab == TAB_CONTROLS);
        COLORREF controlsCol = controlsActive ? textActive : (g_tabControlsHovered ? textHover : textInactive);
        SetTextColor(hdc, controlsCol);
        DrawTextW(hdc, L"\xE790", -1, &g_tabControlsRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        if (controlsActive)
        {
            HBRUSH dotB = CreateSolidBrush(controlsCol);
            HPEN noPen = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, dotB);
            SelectObject(hdc, noPen);
            int dotX = g_tabControlsRect.left + btnW / 2;
            int dotY = g_tabControlsRect.bottom + 1;
            RoundRect(hdc, dotX - 2, dotY, dotX + 2, dotY + 4, 2, 2);
            DeleteObject(dotB);
            DeleteObject(noPen);
        }

        // Actions Tab
        bool actionsActive = (activeTab == TAB_ACTIONS);
        COLORREF actionsCol = actionsActive ? textActive : (g_tabActionsHovered ? textHover : textInactive);
        SetTextColor(hdc, actionsCol);
        DrawTextW(hdc, L"\xE722", -1, &g_tabActionsRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        if (actionsActive)
        {
            HBRUSH dotB = CreateSolidBrush(actionsCol);
            HPEN noPen = CreatePen(PS_NULL, 0, 0);
            SelectObject(hdc, dotB);
            SelectObject(hdc, noPen);
            int dotX = g_tabActionsRect.left + btnW / 2;
            int dotY = g_tabActionsRect.bottom + 1;
            RoundRect(hdc, dotX - 2, dotY, dotX + 2, dotY + 4, 2, 2);
            DeleteObject(dotB);
            DeleteObject(noPen);
        }

        // Music Tab
        if (g_settings.showMusicTab)
        {
            bool musicActive = (activeTab == TAB_MUSIC);
            COLORREF musicCol = musicActive ? textActive : (g_tabMusicHovered ? textHover : textInactive);
            SetTextColor(hdc, musicCol);
            DrawTextW(hdc, L"\xE8D6", -1, &g_tabMusicRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            if (musicActive)
            {
                HBRUSH dotB = CreateSolidBrush(musicCol);
                HPEN noPen = CreatePen(PS_NULL, 0, 0);
                SelectObject(hdc, dotB);
                SelectObject(hdc, noPen);
                int dotX = g_tabMusicRect.left + btnW / 2;
                int dotY = g_tabMusicRect.bottom + 1;
                RoundRect(hdc, dotX - 2, dotY, dotX + 2, dotY + 4, 2, 2);
                DeleteObject(dotB);
                DeleteObject(noPen);
            }
        }

        // Vertical Divider
        int divX = startX + (btnW + gap) * tabCount + divW / 2;
        HPEN divPen = CreatePen(PS_SOLID, 1, isDark ? RGB(65, 70, 85) : RGB(185, 195, 210));
        SelectObject(hdc, divPen);
        MoveToEx(hdc, divX, rowY + 3, nullptr);
        LineTo(hdc, divX, rowY + btnH - 3);
        DeleteObject(divPen);

        // Settings Gear
        int gearX = divX + divW / 2 + gap;
        RECT gearRc = {gearX, rowY, gearX + btnW, rowY + btnH};
        g_settingsBtnRect = gearRc;
        COLORREF gearCol = settingsHovered ? textActive : textInactive;
        SetTextColor(hdc, gearCol);
        DrawTextW(hdc, L"\xE713", -1, &gearRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }

    SelectObject(hdc, oldF);
    DeleteObject(iconFont);
}

int GetQuickControlsDesiredWidth(int dockEdge)
{
    bool isVert = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);
    if (isVert)
        return 260;

    int activeCount = 0;
    if (g_settings.qcShowWifi) activeCount++;
    if (g_settings.qcShowBluetooth) activeCount++;
    if (g_settings.qcShowAirplane) activeCount++;
    if (g_settings.qcShowHotspot) activeCount++;
    if (g_settings.qcShowNightLight) activeCount++;
    if (g_settings.qcShowFocus) activeCount++;
    if (g_settings.qcShowMute) activeCount++;
    if (g_settings.qcShowBattery) activeCount++;
    if (g_settings.qcShowLockPC) activeCount++;

    int slidersW = 0;
    if (g_settings.qcShowBrightness) slidersW += 60;
    if (g_settings.qcShowVolume) slidersW += 60;

    int cols = std::max(1, (activeCount + 1) / 2);
    int tilesW = cols * 100;

    int totalW = 18 + slidersW + (slidersW > 0 ? 14 : 0) + tilesW + 18;
    if (g_settings.qcShowPowerMode)
        totalW = std::max(totalW, 440);
    return std::clamp(totalW, 380, 720);
}

int GetQuickControlsDesiredHeight(int dockEdge)
{
    bool isVert = (dockEdge == EDGE_LEFT || dockEdge == EDGE_RIGHT);
    int activeCount = 0;
    if (g_settings.qcShowWifi) activeCount++;
    if (g_settings.qcShowBluetooth) activeCount++;
    if (g_settings.qcShowAirplane) activeCount++;
    if (g_settings.qcShowHotspot) activeCount++;
    if (g_settings.qcShowNightLight) activeCount++;
    if (g_settings.qcShowFocus) activeCount++;
    if (g_settings.qcShowMute) activeCount++;
    if (g_settings.qcShowBattery) activeCount++;
    if (g_settings.qcShowLockPC) activeCount++;

    bool hasSliders = (g_settings.qcShowBrightness || g_settings.qcShowVolume);

    if (!isVert)
    {
        int h = 34 + 134;
        if (g_settings.qcShowPowerMode)
            h += 44;
        h += 16;
        return h;
    }
    else
    {
        int h = 38;
        if (hasSliders)
            h += 124;
        int rows = (activeCount + 1) / 2;
        h += rows * 70;
        if (g_settings.qcShowPowerMode)
            h += 48;
        h += 16;
        return std::max(180, h);
    }
}

void RenderQuickControls(HDC hdc, int width, int height, bool isDark, int dockEdge)
{
    bool isLeft = (dockEdge == EDGE_LEFT);
    bool isRight = (dockEdge == EDGE_RIGHT);
    bool isVert = (isLeft || isRight);

    int sliderW, sliderH, sliderRadius;
    int pmLeft, pmRight, pmTop, pmBottom;

    struct TileLayout
    {
        RECT rc;
        std::wstring title;
        std::wstring subtitle;
        bool active;
        const wchar_t *iconGlyph;
        bool hasArrow;
        COLORREF activeBg;
        COLORREF activeBadge;
        QuickTileId id;
    };

    TileLayout tileData[MAX_QUICK_TILES] = {};
    g_activeTileCount = 0;

    auto addTile = [&](QuickTileId id, const std::wstring &title, const std::wstring &sub, bool active, const wchar_t *glyph, bool arrow, COLORREF bg, COLORREF badge)
    {
        if (g_activeTileCount < MAX_QUICK_TILES)
        {
            tileData[g_activeTileCount].id = id;
            tileData[g_activeTileCount].title = title;
            tileData[g_activeTileCount].subtitle = sub;
            tileData[g_activeTileCount].active = active;
            tileData[g_activeTileCount].iconGlyph = glyph;
            tileData[g_activeTileCount].hasArrow = arrow;
            tileData[g_activeTileCount].activeBg = bg;
            tileData[g_activeTileCount].activeBadge = badge;
            s_activeTileIds[g_activeTileCount] = id;
            g_activeTileCount++;
        }
    };

    if (g_settings.qcShowWifi)
        addTile(TILE_WIFI, L"Wi-Fi", g_state.wifiOn ? L"On" : L"Off", g_state.wifiOn, L"\xE701", true, RGB(18, 50, 95), RGB(24, 120, 242));
    if (g_settings.qcShowBluetooth)
        addTile(TILE_BLUETOOTH, L"Bluetooth", g_state.bluetoothOn ? L"On" : L"Off", g_state.bluetoothOn, L"\xE702", true, RGB(18, 50, 95), RGB(24, 120, 242));
    if (g_settings.qcShowAirplane)
        addTile(TILE_AIRPLANE, L"Airplane", g_state.airplaneModeOn ? L"On" : L"Off", g_state.airplaneModeOn, L"\xE709", false, RGB(22, 54, 90), RGB(2, 132, 199));
    if (g_settings.qcShowHotspot)
        addTile(TILE_HOTSPOT, L"Hotspot", g_state.hotspotOn ? L"On" : L"Off", g_state.hotspotOn, L"\xE88A", false, RGB(20, 56, 44), RGB(16, 185, 129));
    if (g_settings.qcShowNightLight)
        addTile(TILE_NIGHTLIGHT, L"Night light", g_state.nightLightOn ? L"On" : L"Off", g_state.nightLightOn, L"\xE708", false, RGB(65, 45, 18), RGB(245, 158, 11));
    if (g_settings.qcShowFocus)
        addTile(TILE_FOCUS, L"Focus", g_state.focusOn ? L"On" : L"Off", g_state.focusOn, L"\xE707", false, RGB(55, 28, 80), RGB(139, 92, 246));
    if (g_settings.qcShowMute)
        addTile(TILE_MUTE, L"Mute", g_state.isMuted ? L"Muted" : L"Sound on", g_state.isMuted, g_state.isMuted ? L"\xE74F" : L"\xE767", false, RGB(65, 28, 34), RGB(239, 68, 68));
    if (g_settings.qcShowBattery)
        addTile(TILE_BATTERY, std::to_wstring(g_state.batteryPercent) + L"%", g_state.isPluggedIn ? L"Plugged in" : L"On battery", g_state.isPluggedIn, L"\xE83E", false, RGB(22, 45, 34), RGB(34, 197, 94));
    if (g_settings.qcShowLockPC)
        addTile(TILE_LOCKPC, L"Lock PC", L"Lock session", false, L"\xE72E", false, RGB(38, 42, 54), RGB(100, 110, 130));

    g_wifiArrowRect = {0, 0, 0, 0};
    g_btArrowRect = {0, 0, 0, 0};

    if (!isVert)
    {
        // ==========================================
        // 1. HORIZONTAL DOCK LAYOUT
        // ==========================================
        sliderW = 54;
        sliderH = 132;
        sliderRadius = 22;

        int curX = 18;
        if (g_settings.qcShowBrightness)
        {
            g_brightnessCapsuleRect = {curX, 34, curX + sliderW, 34 + sliderH};
            curX += sliderW + 8;
        }
        else
        {
            g_brightnessCapsuleRect = {0, 0, 0, 0};
        }

        if (g_settings.qcShowVolume)
        {
            g_volumeCapsuleRect = {curX, 34, curX + sliderW, 34 + sliderH};
            curX += sliderW + 14;
        }
        else
        {
            g_volumeCapsuleRect = {0, 0, 0, 0};
            if (g_settings.qcShowBrightness)
                curX += 6;
        }

        int tX = curX;
        int tW = 92;
        int tH = 62;
        int gx = 8;
        int gy = 8;

        for (int i = 0; i < g_activeTileCount; ++i)
        {
            int col = i / 2;
            int row = i % 2;
            int itemX = tX + col * (tW + gx);
            int itemY = 34 + row * (tH + gy);

            tileData[i].rc = {itemX, itemY, itemX + tW, itemY + tH};
            g_tileRects[i] = tileData[i].rc;

            if (tileData[i].id == TILE_WIFI)
                g_wifiArrowRect = {tileData[i].rc.right - 24, tileData[i].rc.top, tileData[i].rc.right, tileData[i].rc.bottom};
            else if (tileData[i].id == TILE_BLUETOOTH)
                g_btArrowRect = {tileData[i].rc.right - 24, tileData[i].rc.top, tileData[i].rc.right, tileData[i].rc.bottom};
        }

        pmLeft = 18;
        pmRight = width - 18;
        pmTop = 174;
        pmBottom = 212;
    }
    else
    {
        // ==========================================
        // 2. VERTICAL DOCK LAYOUT
        // ==========================================
        int cStartX = isLeft ? 44 : 12;
        sliderRadius = 20;

        int curY = 38;
        bool hasBothSliders = (g_settings.qcShowBrightness && g_settings.qcShowVolume);

        if (hasBothSliders)
        {
            sliderW = 98;
            sliderH = 114;
            g_brightnessCapsuleRect = {cStartX, curY, cStartX + sliderW, curY + sliderH};
            g_volumeCapsuleRect = {cStartX + sliderW + 8, curY, cStartX + sliderW * 2 + 8, curY + sliderH};
            curY += sliderH + 10;
        }
        else if (g_settings.qcShowBrightness)
        {
            sliderW = 204;
            sliderH = 114;
            g_brightnessCapsuleRect = {cStartX, curY, cStartX + sliderW, curY + sliderH};
            g_volumeCapsuleRect = {0, 0, 0, 0};
            curY += sliderH + 10;
        }
        else if (g_settings.qcShowVolume)
        {
            sliderW = 204;
            sliderH = 114;
            g_volumeCapsuleRect = {cStartX, curY, cStartX + sliderW, curY + sliderH};
            g_brightnessCapsuleRect = {0, 0, 0, 0};
            curY += sliderH + 10;
        }
        else
        {
            g_brightnessCapsuleRect = {0, 0, 0, 0};
            g_volumeCapsuleRect = {0, 0, 0, 0};
        }

        int tW = 98;
        int tH = 62;
        int gx = 8;
        int gy = 8;

        for (int i = 0; i < g_activeTileCount; ++i)
        {
            int col = i % 2;
            int row = i / 2;
            int itemX = cStartX + col * (tW + gx);
            int itemY = curY + row * (tH + gy);

            tileData[i].rc = {itemX, itemY, itemX + tW, itemY + tH};
            g_tileRects[i] = tileData[i].rc;

            if (tileData[i].id == TILE_WIFI)
                g_wifiArrowRect = {tileData[i].rc.right - 24, tileData[i].rc.top, tileData[i].rc.right, tileData[i].rc.bottom};
            else if (tileData[i].id == TILE_BLUETOOTH)
                g_btArrowRect = {tileData[i].rc.right - 24, tileData[i].rc.top, tileData[i].rc.right, tileData[i].rc.bottom};
        }

        int tileRows = (g_activeTileCount + 1) / 2;
        curY += tileRows * (tH + gy);

        pmLeft = cStartX;
        pmRight = cStartX + tW * 2 + gx;
        pmTop = curY + 4;
        pmBottom = pmTop + 38;
    }

    // A. Render the Capsule Sliders
    auto renderCapsuleSlider = [&](const RECT &r, float level, bool isBrightness, bool isMuted)
    {
        if (r.right <= r.left || r.bottom <= r.top)
            return;

        COLORREF trackBg = isDark ? RGB(32, 35, 45) : RGB(220, 226, 238);
        COLORREF trackBorder = isDark ? RGB(48, 54, 70) : RGB(195, 205, 220);

        HBRUSH tBrush = CreateSolidBrush(trackBg);
        HPEN tPen = CreatePen(PS_SOLID, 1, trackBorder);
        HGDIOBJ oldB = SelectObject(hdc, tBrush);
        HGDIOBJ oldP = SelectObject(hdc, tPen);
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, sliderRadius * 2, sliderRadius * 2);
        SelectObject(hdc, oldB);
        SelectObject(hdc, oldP);
        DeleteObject(tBrush);
        DeleteObject(tPen);

        int rH = r.bottom - r.top;
        int fillH = static_cast<int>(rH * std::clamp(level, 0.0f, 1.0f));
        if (fillH > 0)
        {
            HRGN clipRgn = CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, sliderRadius * 2, sliderRadius * 2);
            SelectClipRgn(hdc, clipRgn);

            COLORREF fillCol = isDark ? RGB(244, 244, 240) : RGB(24, 110, 240);
            HBRUSH fBrush = CreateSolidBrush(fillCol);
            RECT fillRc = {r.left, r.bottom - fillH, r.right, r.bottom};
            FillRect(hdc, &fillRc, fBrush);
            DeleteObject(fBrush);

            SelectClipRgn(hdc, nullptr);
            DeleteObject(clipRgn);
        }

        // Percentage Text with ClearType Segoe UI
        int percentVal = static_cast<int>(std::lround(level * 100.0f));
        std::wstring pctStr = std::to_wstring(percentVal) + L"%";

        bool textCovered = (fillH >= (rH - 24));
        COLORREF textCol = textCovered
                               ? (isDark ? RGB(18, 20, 24) : RGB(255, 255, 255))
                               : (isDark ? RGB(245, 245, 245) : RGB(18, 22, 32));

        HFONT pctFont = CreateClearTypeFont(12, FW_BOLD, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(hdc, pctFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, textCol);
        RECT pctRc = {r.left, r.top + 8, r.right, r.top + 26};
        DrawTextW(hdc, pctStr.c_str(), -1, &pctRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(hdc, oldF);
        DeleteObject(pctFont);

        // Icon with ClearType Segoe MDL2 Assets
        bool iconCovered = (fillH >= 32);
        COLORREF iconCol = iconCovered
                               ? (isDark ? RGB(18, 20, 24) : RGB(255, 255, 255))
                               : (isDark ? RGB(170, 175, 190) : RGB(70, 80, 100));

        HFONT sIconFont = CreateClearTypeFont(18, FW_NORMAL, L"Segoe MDL2 Assets");
        oldF = SelectObject(hdc, sIconFont);
        SetTextColor(hdc, iconCol);
        RECT iconRc = {r.left, r.bottom - 32, r.right, r.bottom - 8};

        const wchar_t *glyph = isBrightness ? L"\xE706" : (isMuted ? L"\xE74F" : L"\xE767");
        DrawTextW(hdc, glyph, -1, &iconRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SelectObject(hdc, oldF);
        DeleteObject(sIconFont);
    };

    if (g_settings.qcShowBrightness)
        renderCapsuleSlider(g_brightnessCapsuleRect, g_state.brightness, true, false);
    if (g_settings.qcShowVolume)
        renderCapsuleSlider(g_volumeCapsuleRect, g_state.volume, false, g_state.isMuted);

    // B. Render the Dynamic Control Tiles
    HFONT titleFont = CreateClearTypeFont(11, FW_SEMIBOLD, L"Segoe UI");
    HFONT subFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe UI");
    HFONT tileIconFont = CreateClearTypeFont(13, FW_NORMAL, L"Segoe MDL2 Assets");

    for (int i = 0; i < g_activeTileCount; ++i)
    {
        const TileLayout &tl = tileData[i];
        const RECT &tr = tl.rc;
        bool isHovered = (g_hoveredTile == i);

        COLORREF tileBg, tileBorder;
        if (tl.id == TILE_MUTE && g_state.isMuted)
        {
            tileBg = isDark
                         ? (isHovered ? RGB(75, 35, 40) : RGB(55, 30, 35))
                         : (isHovered ? RGB(255, 220, 220) : RGB(255, 235, 235));
            tileBorder = isDark ? RGB(120, 55, 60) : RGB(240, 175, 175);
        }
        else if (tl.active && tl.id != TILE_BATTERY)
        {
            tileBg = isDark ? (isHovered ? RGB(24, 60, 110) : tl.activeBg)
                            : (isHovered ? RGB(210, 230, 255) : RGB(225, 238, 255));
            tileBorder = isDark ? RGB(32, 80, 145) : RGB(160, 195, 240);
        }
        else
        {
            tileBg = isDark ? (isHovered ? RGB(36, 40, 52) : RGB(28, 30, 40))
                            : (isHovered ? RGB(216, 222, 234) : RGB(226, 232, 242));
            tileBorder = isDark ? RGB(48, 54, 70) : RGB(195, 205, 220);
        }

        HBRUSH bgBrush = CreateSolidBrush(tileBg);
        HPEN bPen = CreatePen(PS_SOLID, 1, tileBorder);
        HGDIOBJ oldB = SelectObject(hdc, bgBrush);
        HGDIOBJ oldP = SelectObject(hdc, bPen);
        RoundRect(hdc, tr.left, tr.top, tr.right, tr.bottom, 14, 14);
        SelectObject(hdc, oldB);
        SelectObject(hdc, oldP);
        DeleteObject(bgBrush);
        DeleteObject(bPen);

        // Circular Badge at Top-Left
        int badgeX = tr.left + 10;
        int badgeY = tr.top + 9;
        int badgeD = 22;

        COLORREF badgeBg = tl.active ? tl.activeBadge : (isDark ? RGB(44, 48, 62) : RGB(200, 208, 222));
        COLORREF badgeIconCol = tl.active ? RGB(255, 255, 255) : (isDark ? RGB(190, 195, 205) : RGB(60, 70, 88));
        if (tl.id == TILE_MUTE && g_state.isMuted)
        {
            badgeBg = RGB(239, 68, 68);
            badgeIconCol = RGB(255, 255, 255);
        }
        else if (tl.id == TILE_BATTERY)
        {
            badgeBg = isDark ? RGB(30, 46, 38) : RGB(210, 240, 225);
            badgeIconCol = RGB(34, 197, 94);
        }

        HBRUSH badgeBrush = CreateSolidBrush(badgeBg);
        HPEN noPen = CreatePen(PS_NULL, 0, 0);
        SelectObject(hdc, badgeBrush);
        SelectObject(hdc, noPen);
        RoundRect(hdc, badgeX, badgeY, badgeX + badgeD, badgeY + badgeD, badgeD, badgeD);
        DeleteObject(badgeBrush);
        DeleteObject(noPen);

        // ClearType Icon inside Badge
        SelectObject(hdc, tileIconFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, badgeIconCol);
        RECT badgeRc = {badgeX, badgeY, badgeX + badgeD, badgeY + badgeD};
        DrawTextW(hdc, tl.iconGlyph, -1, &badgeRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        // Arrow '>' for Wi-Fi and Bluetooth
        if (tl.hasArrow)
        {
            bool isArrowHover = (tl.id == TILE_WIFI ? g_wifiArrowHovered : g_btArrowHovered);
            RECT aRc = (tl.id == TILE_WIFI ? g_wifiArrowRect : g_btArrowRect);
            if (isArrowHover)
            {
                HBRUSH aHov = CreateSolidBrush(isDark ? RGB(50, 56, 72) : RGB(200, 212, 230));
                HPEN aPen = CreatePen(PS_NULL, 0, 0);
                SelectObject(hdc, aHov);
                SelectObject(hdc, aPen);
                RoundRect(hdc, aRc.left + 2, aRc.top + 8, aRc.right - 2, aRc.bottom - 8, 6, 6);
                DeleteObject(aHov);
                DeleteObject(aPen);
            }

            HFONT arrowFont = CreateClearTypeFont(11, FW_NORMAL, L"Segoe MDL2 Assets");
            HGDIOBJ oldAF = SelectObject(hdc, arrowFont);
            SetTextColor(hdc, isDark ? RGB(200, 215, 230) : RGB(60, 70, 85));
            DrawTextW(hdc, L"\xE76C", -1, &aRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            SelectObject(hdc, oldAF);
            DeleteObject(arrowFont);
        }

        // ClearType Title & Subtitle
        SelectObject(hdc, titleFont);
        SetTextColor(hdc, isDark ? RGB(245, 248, 255) : RGB(18, 22, 32));
        int textRight = tl.hasArrow ? (tr.right - 26) : (tr.right - 6);
        RECT tRc = {tr.left + 10, tr.top + 33, textRight, tr.top + 46};
        DrawTextW(hdc, tl.title.c_str(), -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        SelectObject(hdc, subFont);
        COLORREF subColor = tl.active
                                ? (isDark ? RGB(190, 215, 255) : RGB(20, 95, 210))
                                : (isDark ? RGB(145, 150, 165) : RGB(105, 115, 130));
        SetTextColor(hdc, subColor);
        RECT sRc = {tr.left + 10, tr.top + 46, textRight, tr.top + 59};
        DrawTextW(hdc, tl.subtitle.c_str(), -1, &sRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    DeleteObject(titleFont);
    DeleteObject(subFont);
    DeleteObject(tileIconFont);

    // C. Render the 3-Segment Power Mode Pill (if enabled)
    if (g_settings.qcShowPowerMode)
    {
        HBRUSH pmBrush = CreateSolidBrush(isDark ? RGB(24, 26, 34) : RGB(220, 226, 236));
        HPEN pmPen = CreatePen(PS_SOLID, 1, isDark ? RGB(42, 46, 58) : RGB(195, 205, 220));
        SelectObject(hdc, pmBrush);
        SelectObject(hdc, pmPen);
        RoundRect(hdc, pmLeft, pmTop, pmRight, pmBottom, 18, 18);
        DeleteObject(pmBrush);
        DeleteObject(pmPen);

        int pmW = pmRight - pmLeft;
        int segW = pmW / 3;

        g_powerModeRects[0] = {pmLeft, pmTop, pmLeft + segW, pmBottom};
        g_powerModeRects[1] = {pmLeft + segW, pmTop, pmLeft + segW * 2, pmBottom};
        g_powerModeRects[2] = {pmLeft + segW * 2, pmTop, pmRight, pmBottom};

        const wchar_t *pmLabels[3] = {L"Battery", L"Balanced", L"Performance"};
        int activeSeg = static_cast<int>(g_state.powerMode);

        const RECT &actRc = g_powerModeRects[activeSeg];
        HBRUSH actBrush = CreateSolidBrush(isDark ? RGB(244, 244, 240) : RGB(28, 34, 48));
        HPEN noPen = CreatePen(PS_NULL, 0, 0);
        SelectObject(hdc, actBrush);
        SelectObject(hdc, noPen);
        RoundRect(hdc, actRc.left + 2, actRc.top + 2, actRc.right - 2, actRc.bottom - 2, 16, 16);
        DeleteObject(actBrush);
        DeleteObject(noPen);

        int labelFontSize = isVert ? 10 : 11;
        HFONT boldSegFont = CreateClearTypeFont(labelFontSize, FW_BOLD, L"Segoe UI");
        HFONT normSegFont = CreateClearTypeFont(labelFontSize, FW_NORMAL, L"Segoe UI");

        for (int k = 0; k < 3; ++k)
        {
            bool isCurrent = (k == activeSeg);
            bool isHover = (g_hoveredPowerMode == k);

            SelectObject(hdc, isCurrent ? boldSegFont : normSegFont);
            COLORREF tc;
            if (isCurrent)
                tc = isDark ? RGB(18, 20, 24) : RGB(255, 255, 255);
            else
                tc = isHover ? (isDark ? RGB(215, 220, 230) : RGB(40, 50, 65))
                             : (isDark ? RGB(150, 155, 170) : RGB(100, 110, 125));

            SetTextColor(hdc, tc);
            RECT lblRc = g_powerModeRects[k];
            DrawTextW(hdc, pmLabels[k], -1, &lblRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        }

        DeleteObject(boldSegFont);
        DeleteObject(normSegFont);
    }
    else
    {
        g_powerModeRects[0] = g_powerModeRects[1] = g_powerModeRects[2] = {0, 0, 0, 0};
    }
}

bool HandleQuickControlsClick(HWND hwnd, POINT pt)
{
    // 1. Brightness Capsule
    if (g_settings.qcShowBrightness && PtInRect(&g_brightnessCapsuleRect, pt))
    {
        g_draggingBrightness = true;
        SetCapture(hwnd);
        float h = static_cast<float>(g_brightnessCapsuleRect.bottom - g_brightnessCapsuleRect.top);
        float lvl = 1.0f - static_cast<float>(pt.y - g_brightnessCapsuleRect.top) / h;
        lvl = std::clamp(lvl, 0.0f, 1.0f);
        SetSystemBrightness(lvl);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    // 2. Volume Capsule
    if (g_settings.qcShowVolume && PtInRect(&g_volumeCapsuleRect, pt))
    {
        if (pt.y >= g_volumeCapsuleRect.bottom - 28)
        {
            ToggleSystemMute();
        }
        else
        {
            g_draggingVolume = true;
            SetCapture(hwnd);
            float h = static_cast<float>(g_volumeCapsuleRect.bottom - g_volumeCapsuleRect.top);
            float lvl = 1.0f - static_cast<float>(pt.y - g_volumeCapsuleRect.top) / h;
            SetSystemVolume(lvl);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    // 3. Arrow clicks on Wi-Fi and Bluetooth
    if (PtInRect(&g_wifiArrowRect, pt))
    {
        ShellExecuteW(nullptr, L"open", L"explorer.exe", L"ms-availablenetworks:", nullptr, SW_SHOW);
        return true;
    }
    if (PtInRect(&g_btArrowRect, pt))
    {
        ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOW);
        return true;
    }

    // 4. Main Dynamic Tile Bodies
    for (int i = 0; i < g_activeTileCount; ++i)
    {
        if (PtInRect(&g_tileRects[i], pt))
        {
            switch (s_activeTileIds[i])
            {
            case TILE_WIFI:
                ToggleWifi();
                break;
            case TILE_BLUETOOTH:
                ToggleBluetooth();
                break;
            case TILE_AIRPLANE:
                ToggleAirplaneMode();
                break;
            case TILE_HOTSPOT:
                ToggleMobileHotspot();
                break;
            case TILE_NIGHTLIGHT:
                ToggleNightLight();
                break;
            case TILE_FOCUS:
                ToggleFocus();
                break;
            case TILE_MUTE:
                ToggleSystemMute();
                break;
            case TILE_BATTERY:
                ShellExecuteW(nullptr, L"open", L"ms-settings:powersleep", nullptr, nullptr, SW_SHOW);
                break;
            case TILE_LOCKPC:
                LockWorkStation();
                break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
    }

    // 5. Power Mode Segments
    if (g_settings.qcShowPowerMode)
    {
        for (int k = 0; k < 3; ++k)
        {
            if (PtInRect(&g_powerModeRects[k], pt))
            {
                SetPowerMode(static_cast<PowerMode>(k));
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
        }
    }

    return false;
}

bool HandleQuickControlsMouseMove(HWND hwnd, POINT pt)
{
    if (g_draggingBrightness && g_settings.qcShowBrightness)
    {
        float h = static_cast<float>(g_brightnessCapsuleRect.bottom - g_brightnessCapsuleRect.top);
        float lvl = 1.0f - static_cast<float>(pt.y - g_brightnessCapsuleRect.top) / h;
        lvl = std::clamp(lvl, 0.0f, 1.0f);
        SetSystemBrightness(lvl);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    if (g_draggingVolume && g_settings.qcShowVolume)
    {
        float h = static_cast<float>(g_volumeCapsuleRect.bottom - g_volumeCapsuleRect.top);
        float lvl = 1.0f - static_cast<float>(pt.y - g_volumeCapsuleRect.top) / h;
        SetSystemVolume(lvl);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    bool newWifiArrowH = PtInRect(&g_wifiArrowRect, pt);
    bool newBtArrowH = PtInRect(&g_btArrowRect, pt);
    bool arrowChanged = (newWifiArrowH != g_wifiArrowHovered || newBtArrowH != g_btArrowHovered);
    g_wifiArrowHovered = newWifiArrowH;
    g_btArrowHovered = newBtArrowH;

    int newHoverTile = -1;
    for (int i = 0; i < g_activeTileCount; ++i)
    {
        if (PtInRect(&g_tileRects[i], pt))
        {
            newHoverTile = i;
            break;
        }
    }

    int newHoverPM = -1;
    if (g_settings.qcShowPowerMode)
    {
        for (int k = 0; k < 3; ++k)
        {
            if (PtInRect(&g_powerModeRects[k], pt))
            {
                newHoverPM = k;
                break;
            }
        }
    }

    if (arrowChanged || newHoverTile != g_hoveredTile || newHoverPM != g_hoveredPowerMode)
    {
        g_hoveredTile = newHoverTile;
        g_hoveredPowerMode = newHoverPM;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    return false;
}

void HandleQuickControlsMouseUp(HWND hwnd)
{
    if (g_draggingBrightness || g_draggingVolume)
    {
        g_draggingBrightness = false;
        g_draggingVolume = false;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

bool HandleQuickControlsWheel(HWND hwnd, POINT pt, short delta)
{
    float step = (delta > 0 ? 0.05f : -0.05f);
    if (PtInRect(&g_brightnessCapsuleRect, pt))
    {
        SetSystemBrightness(g_state.brightness + step);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_volumeCapsuleRect, pt))
    {
        SetSystemVolume(g_state.volume + step);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    return false;
}
