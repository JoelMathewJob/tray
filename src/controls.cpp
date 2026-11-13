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
RECT g_settingsBtnRect = {};
RECT g_brightnessCapsuleRect = {};
RECT g_volumeCapsuleRect = {};
RECT g_tileRects[6] = {};
RECT g_wifiArrowRect = {};
RECT g_btArrowRect = {};
RECT g_powerModeRects[3] = {};

bool g_tabFilesHovered = false;
bool g_tabControlsHovered = false;
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

        g_tabFilesRect = {barX, startY, barX + btnW, startY + btnH};
        g_tabControlsRect = {barX, startY + btnH + gap, barX + btnW, startY + (btnH + gap) * 2};
        int divY = startY + (btnH + gap) * 2 + 6;
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

        int totalBarW = btnW + gap + btnW + gap + divW + gap + btnW;
        int startX = (width - totalBarW) / 2;

        g_tabFilesRect = {startX, rowY, startX + btnW, rowY + btnH};
        g_tabControlsRect = {startX + btnW + gap, rowY, startX + btnW + gap + btnW, rowY + btnH};

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

        // Vertical Divider
        int divX = startX + btnW + gap + btnW + gap + divW / 2;
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
    };

    TileLayout tileData[6] = {};

    if (!isVert)
    {
        // ==========================================
        // 1. HORIZONTAL DOCK LAYOUT (Top edge: 450 x 226)
        // ==========================================
        sliderW = 54;
        sliderH = 132;
        sliderRadius = 22;

        g_brightnessCapsuleRect = {18, 34, 18 + sliderW, 34 + sliderH};
        g_volumeCapsuleRect = {78, 34, 78 + sliderW, 34 + sliderH};

        int tX = 140;
        int tW = 92;
        int tH = 62;
        int gx = 8;
        int gy = 8;

        g_tileRects[0] = {tX, 34, tX + tW, 34 + tH};                                               // Wi-Fi
        g_tileRects[1] = {tX + tW + gx, 34, tX + (tW + gx) * 2, 34 + tH};                          // Bluetooth
        g_tileRects[2] = {tX + (tW + gx) * 2, 34, tX + (tW + gx) * 3, 34 + tH};                    // Night Light
        g_tileRects[3] = {tX, 34 + tH + gy, tX + tW, 34 + tH * 2 + gy};                            // Focus
        g_tileRects[4] = {tX + tW + gx, 34 + tH + gy, tX + (tW + gx) * 2, 34 + tH * 2 + gy};       // Mute
        g_tileRects[5] = {tX + (tW + gx) * 2, 34 + tH + gy, tX + (tW + gx) * 3, 34 + tH * 2 + gy}; // Battery

        g_wifiArrowRect = {g_tileRects[0].right - 24, g_tileRects[0].top, g_tileRects[0].right, g_tileRects[0].bottom};
        g_btArrowRect = {g_tileRects[1].right - 24, g_tileRects[1].top, g_tileRects[1].right, g_tileRects[1].bottom};

        pmLeft = 18;
        pmRight = width - 18;
        pmTop = 174;
        pmBottom = 212;
    }
    else
    {
        // ==========================================
        // 2. VERTICAL DOCK LAYOUT (Left or Right Edge: 260 x 416)
        // ==========================================
        // Symmetrical 10px gap between controls and vertical tab bar on both edges
        int cStartX = isLeft ? 44 : 12;
        sliderW = 98;
        sliderH = 116;
        sliderRadius = 20;

        g_brightnessCapsuleRect = {cStartX, 16, cStartX + sliderW, 16 + sliderH};
        g_volumeCapsuleRect = {cStartX + sliderW + 8, 16, cStartX + sliderW * 2 + 8, 16 + sliderH};

        int tW = 98;
        int tH = 64;
        int gx = 8;
        int gy = 8;
        int tStartY = 142;

        g_tileRects[0] = {cStartX, tStartY, cStartX + tW, tStartY + tH};                                                 // Wi-Fi
        g_tileRects[1] = {cStartX + tW + gx, tStartY, cStartX + tW * 2 + gx, tStartY + tH};                              // Bluetooth
        g_tileRects[2] = {cStartX, tStartY + tH + gy, cStartX + tW, tStartY + tH * 2 + gy};                              // Night Light
        g_tileRects[3] = {cStartX + tW + gx, tStartY + tH + gy, cStartX + tW * 2 + gx, tStartY + tH * 2 + gy};           // Focus
        g_tileRects[4] = {cStartX, tStartY + (tH + gy) * 2, cStartX + tW, tStartY + tH * 3 + gy * 2};                    // Mute
        g_tileRects[5] = {cStartX + tW + gx, tStartY + (tH + gy) * 2, cStartX + tW * 2 + gx, tStartY + tH * 3 + gy * 2}; // Battery

        g_wifiArrowRect = {g_tileRects[0].right - 24, g_tileRects[0].top, g_tileRects[0].right, g_tileRects[0].bottom};
        g_btArrowRect = {g_tileRects[1].right - 24, g_tileRects[1].top, g_tileRects[1].right, g_tileRects[1].bottom};

        pmLeft = cStartX;
        pmRight = cStartX + tW * 2 + gx;
        pmTop = 362;
        pmBottom = 402;
    }

    tileData[0] = {g_tileRects[0], L"Wi-Fi", g_state.wifiOn ? L"On" : L"Off", g_state.wifiOn, L"\xE701", true, RGB(18, 50, 95), RGB(24, 120, 242)};
    tileData[1] = {g_tileRects[1], L"Bluetooth", g_state.bluetoothOn ? L"On" : L"Off", g_state.bluetoothOn, L"\xE702", true, RGB(18, 50, 95), RGB(24, 120, 242)};
    tileData[2] = {g_tileRects[2], L"Night light", g_state.nightLightOn ? L"On" : L"Off", g_state.nightLightOn, L"\xE708", false, RGB(65, 45, 18), RGB(245, 158, 11)};
    tileData[3] = {g_tileRects[3], L"Focus", g_state.focusOn ? L"On" : L"Off", g_state.focusOn, L"\xE707", false, RGB(55, 28, 80), RGB(139, 92, 246)};
    tileData[4] = {g_tileRects[4], L"Mute", g_state.isMuted ? L"Muted" : L"Sound on", false, g_state.isMuted ? L"\xE74F" : L"\xE767", false, RGB(18, 50, 95), RGB(239, 68, 68)};
    tileData[5] = {g_tileRects[5], std::to_wstring(g_state.batteryPercent) + L"%", g_state.isPluggedIn ? L"Plugged in" : L"On battery", g_state.isPluggedIn, L"\xE83E", false, RGB(22, 45, 34), RGB(34, 197, 94)};

    // A. Render the Two Vertical Capsule Sliders
    auto renderCapsuleSlider = [&](const RECT &r, float level, bool isBrightness, bool isMuted)
    {
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

    renderCapsuleSlider(g_brightnessCapsuleRect, g_state.brightness, true, false);
    renderCapsuleSlider(g_volumeCapsuleRect, g_state.volume, false, g_state.isMuted);

    // B. Render the 6 Control Tiles
    HFONT titleFont = CreateClearTypeFont(11, FW_SEMIBOLD, L"Segoe UI");
    HFONT subFont = CreateClearTypeFont(10, FW_NORMAL, L"Segoe UI");
    HFONT tileIconFont = CreateClearTypeFont(13, FW_NORMAL, L"Segoe MDL2 Assets");

    for (int i = 0; i < 6; ++i)
    {
        const TileLayout &tl = tileData[i];
        const RECT &tr = tl.rc;
        bool isHovered = (g_hoveredTile == i);

        COLORREF tileBg, tileBorder;
        if (i == 4 && g_state.isMuted)
        {
            // Muted: subtle red background
            tileBg = isDark
                         ? (isHovered ? RGB(75, 35, 40) : RGB(55, 30, 35))
                         : (isHovered ? RGB(255, 220, 220) : RGB(255, 235, 235));

            tileBorder = isDark ? RGB(120, 55, 60) : RGB(240, 175, 175);
        }
        else if (tl.active && (i <= 1 || i == 2 || i == 3))
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
        int badgeX = tr.left + 12;
        int badgeY = tr.top + 10;
        int badgeD = 22;

        COLORREF badgeBg = tl.active ? tl.activeBadge : (isDark ? RGB(44, 48, 62) : RGB(200, 208, 222));
        COLORREF badgeIconCol = tl.active ? RGB(255, 255, 255) : (isDark ? RGB(190, 195, 205) : RGB(60, 70, 88));
        if (i == 4 && g_state.isMuted)
        {
            badgeBg = RGB(239, 68, 68);
            badgeIconCol = RGB(255, 255, 255);
        }
        else if (i == 5)
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
            bool isArrowHover = (i == 0 ? g_wifiArrowHovered : g_btArrowHovered);
            RECT aRc = (i == 0 ? g_wifiArrowRect : g_btArrowRect);
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
        int textRight = tl.hasArrow ? (tr.right - 26) : (tr.right - 8);
        RECT tRc = {tr.left + 10, tr.top + 34, textRight, tr.top + 48};
        DrawTextW(hdc, tl.title.c_str(), -1, &tRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        SelectObject(hdc, subFont);
        COLORREF subColor = tl.active
                                ? (isDark ? RGB(190, 215, 255) : RGB(20, 95, 210))
                                : (isDark ? RGB(145, 150, 165) : RGB(105, 115, 130));
        SetTextColor(hdc, subColor);
        RECT sRc = {tr.left + 10, tr.top + 48, textRight, tr.top + 62};
        DrawTextW(hdc, tl.subtitle.c_str(), -1, &sRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    DeleteObject(titleFont);
    DeleteObject(subFont);
    DeleteObject(tileIconFont);

    // C. Render the 3-Segment Power Mode Pill
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

    // Active Selection Pill
    const RECT &actRc = g_powerModeRects[activeSeg];
    HBRUSH actBrush = CreateSolidBrush(isDark ? RGB(244, 244, 240) : RGB(28, 34, 48));
    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    SelectObject(hdc, actBrush);
    SelectObject(hdc, noPen);
    RoundRect(hdc, actRc.left + 2, actRc.top + 2, actRc.right - 2, actRc.bottom - 2, 16, 16);
    DeleteObject(actBrush);
    DeleteObject(noPen);

    // Segment Labels with ClearType Segoe UI (single line, no wrap)
    int labelFontSize = isVert ? 10 : 12;
    HFONT boldSegFont = CreateClearTypeFont(labelFontSize, FW_BOLD, L"Segoe UI");
    HFONT normSegFont = CreateClearTypeFont(labelFontSize, FW_NORMAL, L"Segoe UI");

    for (int k = 0; k < 3; ++k)
    {
        bool isCurrent = (k == activeSeg);
        bool isHover = (g_hoveredPowerMode == k);

        SelectObject(hdc, isCurrent ? boldSegFont : normSegFont);
        COLORREF tc;
        if (isCurrent)
        {
            tc = isDark ? RGB(18, 20, 24) : RGB(255, 255, 255);
        }
        else
        {
            tc = isHover
                     ? (isDark ? RGB(215, 220, 230) : RGB(40, 50, 65))
                     : (isDark ? RGB(150, 155, 170) : RGB(100, 110, 125));
        }

        SetTextColor(hdc, tc);
        RECT lblRc = g_powerModeRects[k];
        DrawTextW(hdc, pmLabels[k], -1, &lblRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
    }

    DeleteObject(boldSegFont);
    DeleteObject(normSegFont);
}

bool HandleQuickControlsClick(HWND hwnd, POINT pt)
{
    // 1. Brightness Capsule
    if (PtInRect(&g_brightnessCapsuleRect, pt))
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
    if (PtInRect(&g_volumeCapsuleRect, pt))
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

    // 4. Main Tile Bodies (direct in-place toggling)
    if (PtInRect(&g_tileRects[0], pt)) // Wi-Fi body
    {
        ToggleWifi();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_tileRects[1], pt)) // Bluetooth body
    {
        ToggleBluetooth();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_tileRects[2], pt)) // Night light
    {
        ToggleNightLight();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_tileRects[3], pt)) // Focus
    {
        ToggleFocus();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_tileRects[4], pt)) // Mute
    {
        ToggleSystemMute();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (PtInRect(&g_tileRects[5], pt)) // Battery
    {
        ShellExecuteW(nullptr, L"open", L"ms-settings:powersleep", nullptr, nullptr, SW_SHOW);
        return true;
    }

    // 5. Power Mode Segments
    for (int k = 0; k < 3; ++k)
    {
        if (PtInRect(&g_powerModeRects[k], pt))
        {
            SetPowerMode(static_cast<PowerMode>(k));
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
    }

    return false;
}

bool HandleQuickControlsMouseMove(HWND hwnd, POINT pt)
{
    if (g_draggingBrightness)
    {
        float h = static_cast<float>(g_brightnessCapsuleRect.bottom - g_brightnessCapsuleRect.top);
        float lvl = 1.0f -
                    static_cast<float>(pt.y - g_brightnessCapsuleRect.top) / h;

        lvl = std::clamp(lvl, 0.0f, 1.0f);
        SetSystemBrightness(lvl);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    if (g_draggingVolume)
    {
        float h = static_cast<float>(g_volumeCapsuleRect.bottom - g_volumeCapsuleRect.top);
        float lvl = 1.0f - static_cast<float>(pt.y - g_volumeCapsuleRect.top) / h;
        SetSystemVolume(lvl);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    // Hover states for arrow buttons
    bool newWifiArrowH = PtInRect(&g_wifiArrowRect, pt);
    bool newBtArrowH = PtInRect(&g_btArrowRect, pt);
    bool arrowChanged = (newWifiArrowH != g_wifiArrowHovered || newBtArrowH != g_btArrowHovered);
    g_wifiArrowHovered = newWifiArrowH;
    g_btArrowHovered = newBtArrowH;

    // Hover states for tiles
    int newHoverTile = -1;
    for (int i = 0; i < 6; ++i)
    {
        if (PtInRect(&g_tileRects[i], pt))
        {
            newHoverTile = i;
            break;
        }
    }

    // Hover states for power mode
    int newHoverPM = -1;
    for (int k = 0; k < 3; ++k)
    {
        if (PtInRect(&g_powerModeRects[k], pt))
        {
            newHoverPM = k;
            break;
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
