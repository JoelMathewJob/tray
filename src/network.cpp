#include "network.h"
#include "actions.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>

using json = nlohmann::json;

std::vector<DiscoveredPeer> g_peers;
std::mutex g_peersMutex;

static std::string GetInstanceId() {
    static std::string s_id;
    if (s_id.empty()) {
        s_id = std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    }
    return s_id;
}

struct LocalAdapterInfo {
    std::string ip;
    std::string bcast;
    std::string name;
    bool isWifi;
};

static std::vector<LocalAdapterInfo> GetLocalAdapters() {
    std::vector<LocalAdapterInfo> result;
    ULONG bufLen = 16384;
    std::vector<BYTE> buffer(bufLen);
    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufLen);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufLen);
    }

    if (ret != NO_ERROR) return result;

    for (PIP_ADAPTER_ADDRESSES pCurr = adapters; pCurr; pCurr = pCurr->Next) {
        if (pCurr->OperStatus != IfOperStatusUp) continue;
        if (pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        bool isWifi = (pCurr->IfType == IF_TYPE_IEEE80211);
        std::string friendlyName;
        if (pCurr->FriendlyName) {
            int len = WideCharToMultiByte(CP_UTF8, 0, pCurr->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                friendlyName.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, pCurr->FriendlyName, -1, &friendlyName[0], len, nullptr, nullptr);
            }
        }

        for (PIP_ADAPTER_UNICAST_ADDRESS pUni = pCurr->FirstUnicastAddress; pUni; pUni = pUni->Next) {
            sockaddr* sa = pUni->Address.lpSockaddr;
            if (sa && sa->sa_family == AF_INET) {
                sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(sa);
                char ipStr[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));

                // Ignore link-local APIPA 169.254.x.x
                if (strncmp(ipStr, "169.254.", 8) == 0) continue;

                // Calculate subnet mask from OnLinkPrefixLength
                ULONG prefixLen = pUni->OnLinkPrefixLength;
                ULONG mask = (prefixLen == 0 || prefixLen > 32) ? 0xFFFFFF00 : (~0UL << (32 - prefixLen));
                ULONG maskNbo = htonl(mask);
                ULONG ipNbo = sin->sin_addr.s_addr;
                ULONG bcastNbo = ipNbo | (~maskNbo);

                char bcastStr[INET_ADDRSTRLEN] = {0};
                in_addr bcastAddr;
                bcastAddr.s_addr = bcastNbo;
                inet_ntop(AF_INET, &bcastAddr, bcastStr, sizeof(bcastStr));

                LocalAdapterInfo entry;
                entry.ip = ipStr;
                entry.bcast = bcastStr;
                entry.name = friendlyName;
                entry.isWifi = isWifi;
                result.push_back(entry);
            }
        }
    }
    return result;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &str[0], sizeNeeded, nullptr, nullptr);
    return str;
}

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &wstr[0], sizeNeeded);
    return wstr;
}

static std::wstring FormatFileSize(ULONGLONG bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    if (bytes < 1024 * 1024) return std::to_wstring(bytes / 1024) + L" KB";
    return std::to_wstring(bytes / (1024 * 1024)) + L" MB";
}

NetworkManager& NetworkManager::Instance() {
    static NetworkManager s_instance;
    return s_instance;
}

NetworkManager::~NetworkManager() {
    Stop();
}

std::string NetworkManager::GetLocalIP() {
    ULONGLONG now = GetTickCount64();
    if (!m_cachedLocalIp.empty() && (now - m_lastIpCheck < 5000)) {
        return m_cachedLocalIp;
    }
    m_lastIpCheck = now;

    auto adapters = GetLocalAdapters();

    // 1. Prefer Wi-Fi adapter
    for (const auto& a : adapters) {
        if (a.isWifi && !a.ip.empty() && a.ip != "127.0.0.1") {
            m_cachedLocalIp = a.ip;
            return m_cachedLocalIp;
        }
    }

    // 2. Any active LAN adapter
    for (const auto& a : adapters) {
        if (!a.ip.empty() && a.ip != "127.0.0.1") {
            m_cachedLocalIp = a.ip;
            return m_cachedLocalIp;
        }
    }

    // 3. Fallback: probe socket
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s != INVALID_SOCKET) {
        sockaddr_in probeAddr = {};
        probeAddr.sin_family = AF_INET;
        probeAddr.sin_port = htons(9);
        inet_pton(AF_INET, "8.8.8.8", &probeAddr.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&probeAddr), sizeof(probeAddr)) == 0) {
            sockaddr_in localAddr = {};
            int addrLen = sizeof(localAddr);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&localAddr), &addrLen) == 0) {
                char ipStr[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &localAddr.sin_addr, ipStr, sizeof(ipStr));
                if (strlen(ipStr) > 0 && strcmp(ipStr, "0.0.0.0") != 0) {
                    m_cachedLocalIp = ipStr;
                    closesocket(s);
                    return m_cachedLocalIp;
                }
            }
        }
        closesocket(s);
    }

    m_cachedLocalIp = "127.0.0.1";
    return m_cachedLocalIp;
}

bool NetworkManager::Start() {
    if (m_running) return true;
    m_running = true;

    m_udpBroadcastThread = std::thread(&NetworkManager::UdpBroadcastLoop, this);
    m_udpListenThread = std::thread(&NetworkManager::UdpListenLoop, this);
    m_httpServerThread = std::thread(&NetworkManager::HttpServerLoop, this);

    return true;
}

void NetworkManager::Stop() {
    if (!m_running) return;
    m_running = false;

    if (m_httpServer) {
        m_httpServer->stop();
    }

    // Wake up listening socket
    SOCKET wakeSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (wakeSock != INVALID_SOCKET) {
        sockaddr_in loopback = {};
        loopback.sin_family = AF_INET;
        loopback.sin_port = htons(8888);
        inet_pton(AF_INET, "127.0.0.1", &loopback.sin_addr);
        const char* dummy = "EXIT";
        sendto(wakeSock, dummy, 4, 0, reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback));
        closesocket(wakeSock);
    }

    if (m_udpBroadcastThread.joinable()) m_udpBroadcastThread.join();
    if (m_udpListenThread.joinable()) m_udpListenThread.join();
    if (m_httpServerThread.joinable()) m_httpServerThread.join();
}

void NetworkManager::UdpBroadcastLoop() {
    while (m_running) {
        auto adapters = GetLocalAdapters();
        std::string myIp = GetLocalIP();
        std::wstring myName = g_settings.username;
        if (myName.empty()) {
            wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = {0};
            DWORD hlen = MAX_COMPUTERNAME_LENGTH + 1;
            GetComputerNameW(host, &hlen);
            myName = host;
        }

        json j;
        j["app"] = "WinTray";
        j["id"] = GetInstanceId();
        j["name"] = WideToUtf8(myName);
        j["ip"] = myIp;
        j["device"] = "pc";
        j["port"] = GetHttpPort();

        std::string payload = j.dump();

        sockaddr_in mcastAddr = {};
        mcastAddr.sin_family = AF_INET;
        mcastAddr.sin_port = htons(8888);
        inet_pton(AF_INET, "239.255.88.88", &mcastAddr.sin_addr);

        sockaddr_in globalBcastAddr = {};
        globalBcastAddr.sin_family = AF_INET;
        globalBcastAddr.sin_port = htons(8888);
        globalBcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

        if (adapters.empty()) {
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s != INVALID_SOCKET) {
                int bcast = 1;
                setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));
                sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
                       reinterpret_cast<sockaddr*>(&globalBcastAddr), sizeof(globalBcastAddr));
                sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
                       reinterpret_cast<sockaddr*>(&mcastAddr), sizeof(mcastAddr));
                closesocket(s);
            }
        } else {
            for (const auto& a : adapters) {
                SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (s == INVALID_SOCKET) continue;

                int bcast = 1;
                setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

                // Bind to adapter's IP so Windows transmits packets on this specific interface!
                sockaddr_in localSin = {};
                localSin.sin_family = AF_INET;
                localSin.sin_port = 0;
                inet_pton(AF_INET, a.ip.c_str(), &localSin.sin_addr);
                bind(s, reinterpret_cast<sockaddr*>(&localSin), sizeof(localSin));

                // Set multicast interface for this adapter
                in_addr ifAddr = localSin.sin_addr;
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&ifAddr), sizeof(ifAddr));

                // 1. Send to adapter directed subnet broadcast (e.g. 192.168.0.255)
                sockaddr_in subnetBcastAddr = {};
                subnetBcastAddr.sin_family = AF_INET;
                subnetBcastAddr.sin_port = htons(8888);
                inet_pton(AF_INET, a.bcast.c_str(), &subnetBcastAddr.sin_addr);
                sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
                       reinterpret_cast<sockaddr*>(&subnetBcastAddr), sizeof(subnetBcastAddr));

                // 2. Send to global limited broadcast (255.255.255.255)
                sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
                       reinterpret_cast<sockaddr*>(&globalBcastAddr), sizeof(globalBcastAddr));

                // 3. Send to local multicast group (239.255.88.88)
                sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
                       reinterpret_cast<sockaddr*>(&mcastAddr), sizeof(mcastAddr));

                closesocket(s);
            }
        }

        // Sleep 1.5 seconds between discovery beacons
        for (int i = 0; i < 15 && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void NetworkManager::UdpListenLoop() {
    SOCKET listenSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (listenSock == INVALID_SOCKET) return;

    int reuse = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    int bcast = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    DWORD tv = 1500;
    setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(8888);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(listenSock);
        return;
    }

    // Join local multicast group 239.255.88.88
    ip_mreq mreq = {};
    inet_pton(AF_INET, "239.255.88.88", &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(listenSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

    char buf[4096];
    while (m_running) {
        sockaddr_in senderAddr = {};
        int senderLen = sizeof(senderAddr);
        int bytes = recvfrom(listenSock, buf, sizeof(buf) - 1, 0,
                             reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);

        ULONGLONG now = GetTickCount64();

        if (bytes > 0) {
            buf[bytes] = '\0';
            std::string raw(buf, bytes);

            if (raw == "EXIT") {
                continue;
            }

            char actualSenderIp[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &senderAddr.sin_addr, actualSenderIp, sizeof(actualSenderIp));

            try {
                json j = json::parse(raw);
                if (j.value("app", "") == "WinTray") {
                    std::string senderId = j.value("id", "");
                    // Filter out our own beacons using unique instance ID
                    if (!senderId.empty() && senderId == GetInstanceId()) {
                        continue;
                    }

                    std::string peerIp = j.value("ip", "");
                    if (peerIp.empty() || peerIp == "127.0.0.1" || peerIp == "0.0.0.0") {
                        peerIp = actualSenderIp;
                    }

                    // Check that the IP is not one of our own local adapter IPs
                    auto myAdapters = GetLocalAdapters();
                    bool isSelf = false;
                    for (const auto& a : myAdapters) {
                        if (peerIp == a.ip || std::string(actualSenderIp) == a.ip) {
                            isSelf = true;
                            break;
                        }
                    }
                    if (isSelf) continue;

                    std::string peerName = j.value("name", "Unknown PC");
                    std::string peerDev = j.value("device", "pc");
                    int peerPort = j.value("port", 8088);

                    bool peerAdded = false;
                    {
                        std::lock_guard<std::mutex> lock(g_peersMutex);
                        bool found = false;
                        for (auto& p : g_peers) {
                            if (p.ip == peerIp) {
                                p.name = Utf8ToWide(peerName);
                                p.deviceType = peerDev;
                                p.port = peerPort;
                                p.lastSeenTick = now;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            DiscoveredPeer peer;
                            peer.ip = peerIp;
                            peer.name = Utf8ToWide(peerName);
                            peer.deviceType = peerDev;
                            peer.port = peerPort;
                            peer.lastSeenTick = now;
                            g_peers.push_back(peer);
                            peerAdded = true;
                        }
                    }

                    if (peerAdded && g_mainHwnd && IsWindow(g_mainHwnd)) {
                        InvalidateRect(g_mainHwnd, nullptr, FALSE);
                    }
                }
            } catch (...) {}
        }

        // Prune peers older than 10 seconds
        {
            std::lock_guard<std::mutex> lock(g_peersMutex);
            size_t before = g_peers.size();
            g_peers.erase(
                std::remove_if(g_peers.begin(), g_peers.end(), [now](const DiscoveredPeer& p) {
                    return (now - p.lastSeenTick) > 10000;
                }),
                g_peers.end()
            );
            if (g_peers.size() != before && g_mainHwnd && IsWindow(g_mainHwnd)) {
                InvalidateRect(g_mainHwnd, nullptr, FALSE);
            }
        }
    }

    closesocket(listenSock);
}

void NetworkManager::HttpServerLoop() {
    m_httpServer = std::make_unique<httplib::Server>();
    auto& svr = *m_httpServer;

    // GET /favicon.ico -> 204 No Content
    svr.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // GET / -> Mobile Web Portal
    svr.Get("/", [this](const httplib::Request&, httplib::Response& res) {
        std::wstring myName = g_settings.username;
        if (myName.empty()) myName = L"WinTray PC";
        std::string myNameUtf8 = WideToUtf8(myName);

        std::string itemsHtml = "";
        for (const auto& item : g_items) {
            if (item.isLoading) continue;
            std::string nameUtf8 = WideToUtf8(item.name);
            itemsHtml += "<div class='file-card'>";
            itemsHtml += "<div class='file-info'>";
            itemsHtml += "<div class='file-icon'>&#128196;</div>";
            itemsHtml += "<div class='file-name'>" + nameUtf8 + "</div>";
            itemsHtml += "</div>";
            itemsHtml += "<a class='btn-download' href='/download?id=" + std::to_string(item.id) + "'>Download</a>";
            itemsHtml += "</div>";
        }

        if (itemsHtml.empty()) {
            itemsHtml = "<p class='empty-text'>No files currently in WinTray.</p>";
        }

        std::string html = R"raw(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>WinTray AirDrop</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; }
        body { background: #0c0f17; color: #f0f4fc; min-height: 100vh; display: flex; flex-direction: column; align-items: center; padding: 20px 16px; }
        .glass-card { width: 100%; max-width: 440px; background: rgba(26, 32, 48, 0.7); backdrop-filter: blur(24px); -webkit-backdrop-filter: blur(24px); border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 24px; padding: 24px; box-shadow: 0 16px 40px rgba(0, 0, 0, 0.5); margin-bottom: 20px; }
        .header { display: flex; align-items: center; gap: 12px; margin-bottom: 20px; }
        .logo-badge { width: 44px; height: 44px; border-radius: 14px; background: linear-gradient(135deg, #0078d4, #00b4d8); display: flex; align-items: center; justify-content: center; font-size: 22px; box-shadow: 0 4px 12px rgba(0, 180, 216, 0.4); }
        .header-title h1 { font-size: 19px; font-weight: 700; color: #fff; }
        .header-title p { font-size: 13px; color: #94a3b8; }
        .dropzone { border: 2px dashed rgba(0, 180, 216, 0.4); border-radius: 16px; padding: 32px 16px; text-align: center; cursor: pointer; transition: all 0.2s; background: rgba(0, 120, 212, 0.05); }
        .dropzone:active, .dropzone.dragover { border-color: #00b4d8; background: rgba(0, 180, 216, 0.15); }
        .dropzone-icon { font-size: 40px; margin-bottom: 8px; }
        .dropzone-title { font-size: 15px; font-weight: 600; color: #e2e8f0; }
        .dropzone-sub { font-size: 12px; color: #64748b; margin-top: 4px; }
        #fileInput { display: none; }
        .btn-send { display: block; width: 100%; margin-top: 16px; padding: 14px; background: #0078d4; color: #fff; border: none; border-radius: 14px; font-size: 15px; font-weight: 600; cursor: pointer; transition: background 0.2s; }
        .btn-send:hover { background: #0086ea; }
        .status-msg { margin-top: 14px; font-size: 13px; text-align: center; display: none; }
        .status-msg.success { color: #4ade80; display: block; }
        .status-msg.error { color: #f87171; display: block; }
        .progress-bar { width: 100%; height: 6px; background: rgba(255,255,255,0.1); border-radius: 3px; overflow: hidden; margin-top: 12px; display: none; }
        .progress-fill { height: 100%; width: 0%; background: #00b4d8; transition: width 0.2s; }
        .section-title { font-size: 14px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.8px; color: #94a3b8; margin: 8px 0 14px; }
        .file-card { display: flex; align-items: center; justify-content: space-between; padding: 12px 14px; background: rgba(255, 255, 255, 0.04); border: 1px solid rgba(255, 255, 255, 0.06); border-radius: 14px; margin-bottom: 10px; }
        .file-info { display: flex; align-items: center; gap: 10px; overflow: hidden; }
        .file-icon { font-size: 20px; }
        .file-name { font-size: 14px; color: #f1f5f9; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 220px; }
        .btn-download { padding: 6px 14px; background: rgba(0, 180, 216, 0.15); border: 1px solid rgba(0, 180, 216, 0.3); border-radius: 10px; color: #38bdf8; font-size: 13px; text-decoration: none; font-weight: 600; }
        .empty-text { font-size: 13px; color: #64748b; text-align: center; padding: 16px 0; }
    </style>
</head>
<body>
    <div class="glass-card">
        <div class="header">
            <div class="logo-badge">&#128225;</div>
            <div class="header-title">
                <h1>WinTray AirDrop</h1>
                <p>Connected to )raw" + myNameUtf8 + R"raw(</p>
            </div>
        </div>

        <div class="dropzone" id="dropzone" onclick="document.getElementById('fileInput').click()">
            <div class="dropzone-icon">&#128229;</div>
            <div class="dropzone-title">Tap or Drop files here</div>
            <div class="dropzone-sub">Send photos, videos, or documents directly to PC</div>
        </div>
        <input type="file" id="fileInput" multiple>

        <div class="progress-bar" id="progressBar"><div class="progress-fill" id="progressFill"></div></div>
        <button class="btn-send" id="sendBtn" style="display:none;" onclick="uploadSelectedFiles()">Send Files to WinTray</button>
        <div class="status-msg" id="statusMsg"></div>
    </div>

    <div class="glass-card">
        <div class="section-title">Files on )raw" + myNameUtf8 + R"raw(</div>
        <div id="fileList">)raw" + itemsHtml + R"raw(</div>
    </div>

    <script>
        const fileInput = document.getElementById('fileInput');
        const sendBtn = document.getElementById('sendBtn');
        const dropzone = document.getElementById('dropzone');
        const statusMsg = document.getElementById('statusMsg');
        const progressBar = document.getElementById('progressBar');
        const progressFill = document.getElementById('progressFill');

        fileInput.addEventListener('change', () => {
            if (fileInput.files.length > 0) {
                sendBtn.style.display = 'block';
                sendBtn.textContent = `Send ${fileInput.files.length} file(s) to PC`;
            }
        });

        ['dragenter', 'dragover'].forEach(name => {
            dropzone.addEventListener(name, (e) => { e.preventDefault(); dropzone.classList.add('dragover'); });
        });
        ['dragleave', 'drop'].forEach(name => {
            dropzone.addEventListener(name, (e) => { e.preventDefault(); dropzone.classList.remove('dragover'); });
        });
        dropzone.addEventListener('drop', (e) => {
            fileInput.files = e.dataTransfer.files;
            if (fileInput.files.length > 0) {
                sendBtn.style.display = 'block';
                sendBtn.textContent = `Send ${fileInput.files.length} file(s) to PC`;
            }
        });

        function uploadSelectedFiles() {
            if (!fileInput.files.length) return;
            const formData = new FormData();
            for (let i = 0; i < fileInput.files.length; i++) {
                formData.append('file_' + i, fileInput.files[i]);
            }

            progressBar.style.display = 'block';
            statusMsg.className = 'status-msg';
            statusMsg.textContent = 'Uploading...';
            sendBtn.disabled = true;

            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/upload', true);

            xhr.upload.onprogress = (e) => {
                if (e.lengthComputable) {
                    const pct = Math.round((e.loaded / e.total) * 100);
                    progressFill.style.width = pct + '%';
                }
            };

            xhr.onload = () => {
                sendBtn.disabled = false;
                progressBar.style.display = 'none';
                if (xhr.status === 200) {
                    statusMsg.className = 'status-msg success';
                    statusMsg.textContent = 'Transfer completed! Files added to WinTray.';
                    setTimeout(() => { location.reload(); }, 1800);
                } else {
                    statusMsg.className = 'status-msg error';
                    statusMsg.textContent = 'Upload failed: ' + xhr.responseText;
                }
            };

            xhr.onerror = () => {
                sendBtn.disabled = false;
                progressBar.style.display = 'none';
                statusMsg.className = 'status-msg error';
                statusMsg.textContent = 'Network error during upload.';
            };

            xhr.send(formData);
        }
    </script>
</body>
</html>)raw";

        res.set_content(html, "text/html; charset=utf-8");
    });

    // POST /upload -> Receive files from mobile or other PC
    svr.Post("/upload", [](const httplib::Request& req, httplib::Response& res) {
        std::vector<httplib::MultipartFormData> uploadList = req.file_list;
        if (uploadList.empty()) {
            for (const auto& pair : req.files) {
                uploadList.push_back(pair.second);
            }
        }

        if (uploadList.empty()) {
            res.status = 400;
            res.set_content("No files attached", "text/plain");
            return;
        }

        g_transfer.isTransferring = true;
        g_transfer.isSending = false;
        g_transfer.currentFileName = L"Receiving files...";
        g_transfer.progress = 0.1f;
        g_transfer.startTime = GetTickCount64();

        if (g_mainHwnd && IsWindow(g_mainHwnd)) {
            InvalidateRect(g_mainHwnd, nullptr, FALSE);
        }

        std::wstring tempDir = GetWinTrayTempDir();
        CreateDirectoryW(tempDir.c_str(), nullptr);

        std::wstring lastFileName = L"";
        size_t totalFiles = uploadList.size();
        size_t fileIdx = 0;

        for (const auto& file : uploadList) {
            std::string fileName = file.filename;
            if (fileName.empty()) fileName = "shared_file_" + std::to_string(GetTickCount64()) + "_" + std::to_string(fileIdx);

            std::wstring wFileName = Utf8ToWide(fileName);
            lastFileName = wFileName;
            g_transfer.currentFileName = wFileName;
            g_transfer.progress = static_cast<float>(fileIdx + 1) / static_cast<float>(totalFiles);
            fileIdx++;

            std::wstring savePath = tempDir + L"\\" + wFileName;

            std::ofstream out(savePath, std::ios::binary);
            if (out.is_open()) {
                out.write(file.content.data(), file.content.size());
                out.close();

                // Add to tray dock
                AddFileToTray(savePath, false, wFileName);
            }
        }

        g_transfer.isTransferring = false;
        g_transfer.progress = 1.0f;

        // Trigger Notification Toast and completion sound
        std::wstring notifMsg = (totalFiles > 1)
            ? (L"Received " + std::to_wstring(totalFiles) + L" files from phone!")
            : (L"Received: " + lastFileName);
        ShowActionToast(notifMsg, L"\xE896", RGB(52, 199, 89));

        if (g_mainHwnd && IsWindow(g_mainHwnd)) {
            CalculateTargetSize(g_isExpanded);
            InvalidateRect(g_mainHwnd, nullptr, FALSE);
        }

        json resp;
        resp["success"] = true;
        resp["message"] = "Files uploaded successfully";
        res.set_content(resp.dump(), "application/json");
    });

    // GET /download?id=X -> Download file from WinTray
    svr.Get("/download", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("id")) {
            res.status = 400;
            res.set_content("Missing item ID", "text/plain");
            return;
        }

        int itemId = std::stoi(req.get_param_value("id"));
        std::wstring filePath;
        std::wstring fileName;

        for (const auto& it : g_items) {
            if (it.id == itemId) {
                filePath = it.path;
                fileName = it.name;
                break;
            }
        }

        if (filePath.empty()) {
            res.status = 404;
            res.set_content("Item not found", "text/plain");
            return;
        }

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            res.status = 404;
            res.set_content("Cannot open file on host", "text/plain");
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        std::string nameUtf8 = WideToUtf8(fileName);
        res.set_header("Content-Disposition", "attachment; filename=\"" + nameUtf8 + "\"");
        res.set_content(content, "application/octet-stream");
    });

    // POST /request-transfer -> PC-to-PC Handshake prompt
    svr.Post("/request-transfer", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json j = json::parse(req.body);
            std::string senderName = j.value("name", "Nearby Device");
            std::string fileName = j.value("filename", "Shared File");
            long long fileSize = j.value("size", 0LL);

            std::wstring prompt = L"Incoming AirDrop file transfer:\n\n"
                                  L"From: " + Utf8ToWide(senderName) + L"\n"
                                  L"File: " + Utf8ToWide(fileName) + L" (" + FormatFileSize(fileSize) + L")\n\n"
                                  L"Do you want to accept and receive this file?";

            int choice = MessageBoxW(g_mainHwnd, prompt.c_str(), L"WinTray AirDrop Transfer",
                                     MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);

            json resp;
            resp["accepted"] = (choice == IDYES);
            res.set_content(resp.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content("{\"accepted\":false}", "application/json");
        }
    });

    std::vector<int> candidates = {8088, 8090, 8880, 8000, 58080, 0};
    int boundPort = svr.bind_to_any_port("0.0.0.0", candidates);
    if (boundPort > 0) {
        m_httpPort = boundPort;
        svr.listen_after_bind();
    }
}

bool NetworkManager::SendFileToPeer(const std::string& peerIp, const std::wstring& filePath, std::string& outError, int peerPort) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        outError = "Could not open source file.";
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::string content(fileSize, '\0');
    file.read(&content[0], fileSize);
    file.close();

    // Extract bare filename
    size_t lastSlash = filePath.find_last_of(L"\\/");
    std::wstring bareName = (lastSlash == std::wstring::npos) ? filePath : filePath.substr(lastSlash + 1);

    std::wstring myName = g_settings.username;
    if (myName.empty()) {
        wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = {0};
        DWORD hlen = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(host, &hlen);
        myName = host;
    }

    // Step 1: Handshake request-transfer
    int targetPort = (peerPort > 0) ? peerPort : GetHttpPort();
    httplib::Client cli(peerIp, targetPort);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(30);

    json reqJ;
    reqJ["name"] = WideToUtf8(myName);
    reqJ["filename"] = WideToUtf8(bareName);
    reqJ["size"] = static_cast<long long>(fileSize);

    auto res = cli.Post("/request-transfer", reqJ.dump(), "application/json");
    if (!res || res.status != 200) {
        outError = "Peer did not respond to transfer request.";
        return false;
    }

    try {
        json respJ = json::parse(res.body);
        if (!respJ.value("accepted", false)) {
            outError = "Transfer was declined by peer.";
            return false;
        }
    } catch (...) {
        outError = "Invalid response from peer.";
        return false;
    }

    // Step 2: Stream / Upload file via multipart POST /upload
    g_transfer.isTransferring = true;
    g_transfer.isSending = true;
    g_transfer.currentFileName = bareName;
    g_transfer.peerName = Utf8ToWide(peerIp);
    g_transfer.progress = 0.5f;
    g_transfer.startTime = GetTickCount64();

    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }

    httplib::MultipartFormData item;
    item.name = "file";
    item.filename = WideToUtf8(bareName);
    item.content = std::move(content);
    item.content_type = "application/octet-stream";

    httplib::MultipartFormDataItems items;
    items.push_back(std::move(item));

    auto uploadRes = cli.Post("/upload", items);

    g_transfer.isTransferring = false;
    g_transfer.progress = 1.0f;

    if (!uploadRes || uploadRes.status != 200) {
        outError = "Upload failed during streaming.";
        ShowActionToast(L"Send failed: " + bareName, L"\xE711", RGB(239, 68, 68));
        if (g_mainHwnd && IsWindow(g_mainHwnd)) {
            InvalidateRect(g_mainHwnd, nullptr, FALSE);
        }
        return false;
    }

    ShowActionToast(L"AirDrop Sent: " + bareName, L"\xE724", RGB(0, 160, 255));
    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
    return true;
}
