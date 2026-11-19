#include "network.h"
#include "actions.h"
#include "notifications_ui.h"
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

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    m_udpBroadcastThread = std::thread(&NetworkManager::UdpBroadcastLoop, this);
    m_udpListenThread = std::thread(&NetworkManager::UdpListenLoop, this);
    m_httpServerThread = std::thread(&NetworkManager::HttpServerLoop, this);
    m_subnetScanThread = std::thread(&NetworkManager::SubnetScanLoop, this);

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
    if (m_subnetScanThread.joinable()) m_subnetScanThread.join();
    if (m_httpServerThread.joinable()) m_httpServerThread.join();

    WSACleanup();
}

void NetworkManager::TriggerQuickScan() {
    m_scanRequested = true;
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

    // GET /api/ping -> High-reliability LAN PC discovery endpoint
    svr.Get("/api/ping", [this](const httplib::Request& req, httplib::Response& res) {
        std::string remoteIp = req.remote_addr;
        std::string myIp = GetLocalIP();
        if (!remoteIp.empty() && remoteIp != "127.0.0.1" && remoteIp != myIp) {
            int remotePort = 8088;
            if (req.has_header("X-WinTray-Port")) {
                try { remotePort = std::stoi(req.get_header_value("X-WinTray-Port")); } catch (...) {}
            }
            std::string senderName = req.has_header("X-WinTray-Name") ? req.get_header_value("X-WinTray-Name") : "";
            std::string senderId = req.has_header("X-WinTray-Id") ? req.get_header_value("X-WinTray-Id") : "";
            if (senderId != GetInstanceId()) {
                if (senderName.empty()) senderName = "Nearby PC";
                ULONGLONG now = GetTickCount64();
                bool peerAdded = false;
                {
                    std::lock_guard<std::mutex> lock(g_peersMutex);
                    bool found = false;
                    for (auto& p : g_peers) {
                        if (p.ip == remoteIp) {
                            p.name = Utf8ToWide(senderName);
                            p.deviceType = "pc";
                            p.port = remotePort;
                            p.lastSeenTick = now;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        DiscoveredPeer peer;
                        peer.ip = remoteIp;
                        peer.name = Utf8ToWide(senderName);
                        peer.deviceType = "pc";
                        peer.port = remotePort;
                        peer.lastSeenTick = now;
                        g_peers.push_back(peer);
                        peerAdded = true;
                    }
                }
                if (peerAdded && g_mainHwnd && IsWindow(g_mainHwnd)) {
                    InvalidateRect(g_mainHwnd, nullptr, FALSE);
                }
            }
        }

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

        res.set_content(j.dump(), "application/json");
    });

    // GET / -> Mobile Web Portal
    svr.Get("/", [this](const httplib::Request&, httplib::Response& res) {
        std::wstring myName = g_settings.username;
        if (myName.empty()) myName = L"WinTray PC";
        std::string myNameUtf8 = WideToUtf8(myName);

        auto escapeHtml = [](const std::string& data) {
            std::string buffer;
            buffer.reserve(data.size());
            for (char c : data) {
                switch (c) {
                    case '&':  buffer.append("&amp;");       break;
                    case '\"': buffer.append("&quot;");      break;
                    case '\'': buffer.append("&#39;");       break;
                    case '<':  buffer.append("&lt;");        break;
                    case '>':  buffer.append("&gt;");        break;
                    default:   buffer.push_back(c);          break;
                }
            }
            return buffer;
        };

        std::string itemsHtml = "";
        for (const auto& item : g_items) {
            if (item.isLoading) continue;

            // Extract disk filename & extension to guarantee correct file type on phone
            std::wstring realFileName = item.name;
            size_t lastSlash = item.path.find_last_of(L"\\/");
            std::wstring diskFileName = (lastSlash != std::wstring::npos) ? item.path.substr(lastSlash + 1) : item.path;
            size_t diskDot = diskFileName.find_last_of(L".");
            if (diskDot != std::wstring::npos && realFileName.find_last_of(L".") == std::wstring::npos) {
                realFileName += diskFileName.substr(diskDot);
            }
            if (realFileName.empty()) realFileName = diskFileName;

            std::string nameUtf8 = WideToUtf8(realFileName);
            std::string safeName = escapeHtml(nameUtf8);
            std::string itemIdStr = std::to_string(item.id);

            itemsHtml += "<div class='file-card'>";
            itemsHtml += "<div class='file-info'>";
            itemsHtml += "<div class='file-icon'>&#128196;</div>";
            itemsHtml += "<div class='file-name'>" + safeName + "</div>";
            itemsHtml += "</div>";
            itemsHtml += "<div class='file-actions'>";
            itemsHtml += "<button class='btn-download' data-id='" + itemIdStr + "' data-name='" + safeName + "' onclick='triggerDownload(this)'>Download</button>";
            itemsHtml += "<a class='btn-direct' href='/download?id=" + itemIdStr + "' download='" + safeName + "' title='Direct Save'>&#11015;</a>";
            itemsHtml += "</div>";
            itemsHtml += "</div>";
        }

        if (itemsHtml.empty()) {
            itemsHtml = "<p class='empty-text'>No files currently in WinTray.</p>";
        } else {
            itemsHtml += "<p class='security-tip'>&#128274; Safe local network transfer. If your browser prompts on download, choose &quot;Keep anyway&quot;.</p>";
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
        .file-actions { display: flex; align-items: center; gap: 8px; }
        .btn-download { padding: 6px 14px; background: rgba(0, 180, 216, 0.18); border: 1px solid rgba(0, 180, 216, 0.35); border-radius: 10px; color: #38bdf8; font-size: 13px; text-decoration: none; font-weight: 600; cursor: pointer; transition: all 0.2s; outline: none; }
        .btn-download:hover { background: #0078d4; color: #fff; }
        .btn-download:disabled { opacity: 0.6; cursor: wait; }
        .btn-direct { display: flex; align-items: center; justify-content: center; width: 30px; height: 30px; background: rgba(255, 255, 255, 0.08); border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 8px; color: #94a3b8; font-size: 14px; text-decoration: none; transition: all 0.2s; }
        .btn-direct:hover { background: rgba(0, 180, 216, 0.25); color: #38bdf8; border-color: rgba(0, 180, 216, 0.4); }
        .security-tip { font-size: 11px; color: #64748b; margin-top: 10px; text-align: center; }
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

        async function triggerDownload(btn) {
            const id = btn.getAttribute('data-id');
            const filename = btn.getAttribute('data-name');
            const origText = btn.textContent;
            btn.textContent = 'Saving...';
            btn.disabled = true;
            try {
                const url = '/download?id=' + encodeURIComponent(id);
                const resp = await fetch(url);
                if (!resp.ok) throw new Error('HTTP ' + resp.status);
                
                const contentType = resp.headers.get('content-type') || 'application/octet-stream';
                const blobData = await resp.blob();
                
                // Construct blob with proper MIME type so mobile OS preserves the exact extension
                const typedBlob = new Blob([blobData], { type: contentType });
                const blobUrl = URL.createObjectURL(typedBlob);
                
                const a = document.createElement('a');
                a.style.display = 'none';
                a.href = blobUrl;
                a.download = filename;
                document.body.appendChild(a);
                a.click();
                
                setTimeout(() => {
                    document.body.removeChild(a);
                    URL.revokeObjectURL(blobUrl);
                }, 4000);
                
                btn.textContent = 'Saved!';
                setTimeout(() => {
                    btn.textContent = origText;
                    btn.disabled = false;
                }, 2000);
            } catch (e) {
                console.warn('Fetch download failed, falling back to direct navigation:', e);
                // Fallback: direct download link
                const fallbackLink = document.createElement('a');
                fallbackLink.href = '/download?id=' + encodeURIComponent(id);
                fallbackLink.download = filename;
                document.body.appendChild(fallbackLink);
                fallbackLink.click();
                setTimeout(() => { document.body.removeChild(fallbackLink); }, 1000);
                
                btn.textContent = origText;
                btn.disabled = false;
            }
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

        // Mark notification card as completed / received
        MarkNotificationsCompleted(req.remote_addr);

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

        int itemId = 0;
        try {
            itemId = std::stoi(req.get_param_value("id"));
        } catch (...) {
            res.status = 400;
            res.set_content("Invalid item ID", "text/plain");
            return;
        }

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

        // Determine true disk filename and extension
        size_t lastSlash = filePath.find_last_of(L"\\/");
        std::wstring diskFileName = (lastSlash != std::wstring::npos) ? filePath.substr(lastSlash + 1) : filePath;
        size_t diskDot = diskFileName.find_last_of(L".");
        std::wstring realExtension = (diskDot != std::wstring::npos) ? diskFileName.substr(diskDot) : L"";

        // Guarantee fileName has the real extension so phone OS never falls back to .bin
        if (!realExtension.empty() && fileName.find_last_of(L".") == std::wstring::npos) {
            fileName += realExtension;
        }
        if (fileName.empty()) {
            fileName = diskFileName;
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

        // Percent-encode for RFC 5987 / 6266 filename*=UTF-8''
        std::string urlEncodedName = "";
        for (unsigned char c : nameUtf8) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') {
                urlEncodedName += c;
            } else {
                char hex[8];
                snprintf(hex, sizeof(hex), "%%%02X", c);
                urlEncodedName += hex;
            }
        }

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Expose-Headers", "Content-Disposition, Content-Type, Content-Length");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("Content-Disposition", "attachment; filename=\"" + nameUtf8 + "\"; filename*=UTF-8''" + urlEncodedName);

        // Determine comprehensive MIME type from real file extension
        std::string ext = "";
        if (!realExtension.empty()) {
            std::wstring wext = realExtension;
            for (auto& c : wext) c = towlower(c);
            ext = WideToUtf8(wext);
        } else {
            size_t dot = fileName.find_last_of(L".");
            if (dot != std::wstring::npos) {
                std::wstring wext = fileName.substr(dot);
                for (auto& c : wext) c = towlower(c);
                ext = WideToUtf8(wext);
            }
        }

        std::string mime = "application/octet-stream";
        if (ext == ".jpg" || ext == ".jpeg") mime = "image/jpeg";
        else if (ext == ".png") mime = "image/png";
        else if (ext == ".gif") mime = "image/gif";
        else if (ext == ".webp") mime = "image/webp";
        else if (ext == ".svg") mime = "image/svg+xml";
        else if (ext == ".bmp") mime = "image/bmp";
        else if (ext == ".ico") mime = "image/x-icon";
        else if (ext == ".pdf") mime = "application/pdf";
        else if (ext == ".mp4") mime = "video/mp4";
        else if (ext == ".mov") mime = "video/quicktime";
        else if (ext == ".mkv") mime = "video/x-matroska";
        else if (ext == ".webm") mime = "video/webm";
        else if (ext == ".avi") mime = "video/x-msvideo";
        else if (ext == ".mp3") mime = "audio/mpeg";
        else if (ext == ".m4a") mime = "audio/mp4";
        else if (ext == ".wav") mime = "audio/wav";
        else if (ext == ".flac") mime = "audio/flac";
        else if (ext == ".ogg" || ext == ".opus") mime = "audio/ogg";
        else if (ext == ".zip") mime = "application/zip";
        else if (ext == ".rar") mime = "application/vnd.rar";
        else if (ext == ".7z") mime = "application/x-7z-compressed";
        else if (ext == ".tar" || ext == ".gz") mime = "application/gzip";
        else if (ext == ".doc") mime = "application/msword";
        else if (ext == ".docx") mime = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
        else if (ext == ".xls") mime = "application/vnd.ms-excel";
        else if (ext == ".xlsx") mime = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
        else if (ext == ".ppt") mime = "application/vnd.ms-powerpoint";
        else if (ext == ".pptx") mime = "application/vnd.openxmlformats-officedocument.presentationml.presentation";
        else if (ext == ".txt") mime = "text/plain; charset=utf-8";
        else if (ext == ".csv") mime = "text/csv; charset=utf-8";
        else if (ext == ".json") mime = "application/json";
        else if (ext == ".html" || ext == ".htm") mime = "text/html; charset=utf-8";
        else if (ext == ".apk") mime = "application/vnd.android.package-archive";

        res.set_content(content, mime.c_str());
    });

    // POST /request-transfer -> PC-to-PC Handshake (Tray Notification)
    svr.Post("/request-transfer", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            json j = json::parse(req.body);
            std::string senderName = j.value("name", "Nearby Device");
            std::string fileName = j.value("filename", "Shared File");
            int fileCount = j.value("count", 1);
            long long fileSize = j.value("size", 0LL);

            auto decision = std::make_shared<std::atomic<int>>(0);
            std::string notifId = std::to_string(GetTickCount64()) + "_" + std::to_string(rand());

            AddTransferNotification(notifId, Utf8ToWide(senderName), req.remote_addr,
                                    Utf8ToWide(fileName), static_cast<ULONGLONG>(fileSize), decision, fileCount);

            // Wait for user decision via Notifications tab in tray (up to 60 seconds)
            int waitedMs = 0;
            while (waitedMs < 60000 && decision->load() == 0 && m_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                waitedMs += 100;
            }

            int choice = decision->load();
            bool accepted = (choice == 1);
            bool expired = (choice == 3 || (choice == 0 && waitedMs >= 60000));

            if (expired) {
                std::lock_guard<std::mutex> lock(g_notificationsMutex);
                for (auto& item : g_notifications) {
                    if (item.id == notifId && item.isPending) {
                        item.isPending = false;
                        item.isExpired = true;
                        item.statusText = L"Expired (Timeout)";
                        break;
                    }
                }
            }

            json resp;
            resp["accepted"] = accepted;
            resp["expired"] = expired;
            res.set_content(resp.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content("{\"accepted\":false,\"expired\":false}", "application/json");
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
    bool dummyExpired = false;
    return SendFilesToPeer(peerIp, {filePath}, outError, dummyExpired, peerPort);
}

bool NetworkManager::SendFilesToPeer(const std::string& peerIp, const std::vector<std::wstring>& filePaths, std::string& outError, bool& outExpired, int peerPort) {
    outExpired = false;
    if (filePaths.empty()) {
        outError = "No files selected to send.";
        return false;
    }

    ULONGLONG totalSize = 0;
    std::vector<std::pair<std::wstring, ULONGLONG>> validFiles;
    for (const auto& fp : filePaths) {
        std::ifstream testF(fp, std::ios::binary | std::ios::ate);
        if (testF.is_open()) {
            ULONGLONG sz = static_cast<ULONGLONG>(testF.tellg());
            totalSize += sz;
            size_t lastS = fp.find_last_of(L"\\/");
            std::wstring bName = (lastS == std::wstring::npos) ? fp : fp.substr(lastS + 1);
            validFiles.push_back({bName, sz});
        }
    }

    if (validFiles.empty()) {
        outError = "Could not open selected file(s).";
        return false;
    }

    std::wstring displayFileName;
    if (validFiles.size() == 1) {
        displayFileName = validFiles[0].first;
    } else {
        displayFileName = validFiles[0].first + L" (+" + std::to_wstring(validFiles.size() - 1) + L" files)";
    }

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
    cli.set_connection_timeout(6);
    cli.set_read_timeout(65); // Give ample time for receiver's 60s prompt

    json reqJ;
    reqJ["name"] = WideToUtf8(myName);
    reqJ["filename"] = WideToUtf8(displayFileName);
    reqJ["count"] = static_cast<int>(validFiles.size());
    reqJ["size"] = static_cast<long long>(totalSize);

    auto res = cli.Post("/request-transfer", reqJ.dump(), "application/json");
    if (!res || res.status != 200) {
        outError = "Peer did not respond (Request timed out).";
        outExpired = true;
        ShowActionToast(L"AirDrop expired: Peer did not respond", L"\xE7BA", RGB(239, 68, 68));
        return false;
    }

    try {
        json respJ = json::parse(res.body);
        bool isAccepted = respJ.value("accepted", false);
        bool isExp = respJ.value("expired", false);

        if (isExp) {
            outExpired = true;
            outError = "Transfer expired: Peer did not respond within 1 min.";
            ShowActionToast(L"AirDrop expired: Peer timed out", L"\xE7BA", RGB(239, 68, 68));
            return false;
        }

        if (!isAccepted) {
            outError = "Transfer was declined by peer.";
            ShowActionToast(L"AirDrop Declined: " + displayFileName, L"\xE711", RGB(239, 68, 68));
            return false;
        }

        ShowActionToast(L"AirDrop Accepted! Sending " + displayFileName, L"\xE73E", RGB(52, 199, 89));
    } catch (...) {
        outError = "Invalid response from peer.";
        return false;
    }

    // Step 2: Stream / Upload files via multipart POST /upload
    g_transfer.isTransferring = true;
    g_transfer.isSending = true;
    g_transfer.currentFileName = displayFileName;
    g_transfer.peerName = Utf8ToWide(peerIp);
    g_transfer.progress = 0.1f;
    g_transfer.startTime = GetTickCount64();

    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }

    httplib::MultipartFormDataItems items;
    for (size_t i = 0; i < filePaths.size(); ++i) {
        const auto& fp = filePaths[i];
        std::ifstream file(fp, std::ios::binary);
        if (!file.is_open()) continue;

        file.seekg(0, std::ios::end);
        size_t fileSize = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        std::string content(fileSize, '\0');
        file.read(&content[0], fileSize);
        file.close();

        size_t lastSlash = fp.find_last_of(L"\\/");
        std::wstring bareName = (lastSlash == std::wstring::npos) ? fp : fp.substr(lastSlash + 1);

        httplib::MultipartFormData item;
        item.name = "file_" + std::to_string(i);
        item.filename = WideToUtf8(bareName);
        item.content = std::move(content);
        item.content_type = "application/octet-stream";
        items.push_back(std::move(item));
    }

    auto uploadRes = cli.Post("/upload", items);

    g_transfer.isTransferring = false;
    g_transfer.progress = 1.0f;

    if (!uploadRes || uploadRes.status != 200) {
        outError = "Upload failed during streaming.";
        ShowActionToast(L"Send failed: " + displayFileName, L"\xE711", RGB(239, 68, 68));
        if (g_mainHwnd && IsWindow(g_mainHwnd)) {
            InvalidateRect(g_mainHwnd, nullptr, FALSE);
        }
        return false;
    }

    ShowActionToast(L"AirDrop Sent: " + displayFileName, L"\xE724", RGB(0, 160, 255));
    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
    return true;
}

void NetworkManager::PingPeerHttp(const std::string& peerIp, int peerPort) {
    httplib::Client cli(peerIp, peerPort);
    cli.set_connection_timeout(1);
    cli.set_read_timeout(1);

    std::wstring myName = g_settings.username;
    if (myName.empty()) {
        wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = {0};
        DWORD hlen = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(host, &hlen);
        myName = host;
    }

    httplib::Headers headers = {
        {"X-WinTray-Port", std::to_string(GetHttpPort())},
        {"X-WinTray-Name", WideToUtf8(myName)},
        {"X-WinTray-Id", GetInstanceId()}
    };

    auto res = cli.Get("/api/ping", headers);
    if (res && res.status == 200) {
        try {
            json j = json::parse(res.body);
            if (j.value("app", "") == "WinTray") {
                std::string senderId = j.value("id", "");
                if (!senderId.empty() && senderId == GetInstanceId()) return;

                std::string peerName = j.value("name", "Nearby PC");
                std::string peerDev = j.value("device", "pc");
                int port = j.value("port", peerPort);
                ULONGLONG now = GetTickCount64();

                bool peerAdded = false;
                {
                    std::lock_guard<std::mutex> lock(g_peersMutex);
                    bool found = false;
                    for (auto& p : g_peers) {
                        if (p.ip == peerIp) {
                            p.name = Utf8ToWide(peerName);
                            p.deviceType = peerDev;
                            p.port = port;
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
                        peer.port = port;
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
}

void NetworkManager::SubnetScanLoop() {
    while (m_running) {
        auto adapters = GetLocalAdapters();
        std::string myIp = GetLocalIP();

        for (const auto& a : adapters) {
            if (!m_running) break;
            if (a.ip.empty() || a.ip == "127.0.0.1") continue;

            size_t lastDot = a.ip.find_last_of('.');
            if (lastDot == std::string::npos) continue;
            std::string prefix = a.ip.substr(0, lastDot + 1);

            int myLastOctet = 0;
            try {
                myLastOctet = std::stoi(a.ip.substr(lastDot + 1));
            } catch (...) {}

            const int scanPort = 8088;

            // Batch in chunks of 60 sockets to stay safely within FD_SETSIZE (64)
            for (int octetStart = 1; octetStart <= 254 && m_running; octetStart += 60) {
                int octetEnd = std::min(octetStart + 59, 254);
                std::vector<std::pair<SOCKET, std::string>> sockList;

                for (int octet = octetStart; octet <= octetEnd; ++octet) {
                    if (octet == myLastOctet) continue;

                    std::string targetIp = prefix + std::to_string(octet);
                    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (s == INVALID_SOCKET) continue;

                    u_long nonBlocking = 1;
                    ioctlsocket(s, FIONBIO, &nonBlocking);

                    sockaddr_in targetAddr = {};
                    targetAddr.sin_family = AF_INET;
                    targetAddr.sin_port = htons(scanPort);
                    inet_pton(AF_INET, targetIp.c_str(), &targetAddr.sin_addr);

                    connect(s, reinterpret_cast<sockaddr*>(&targetAddr), sizeof(targetAddr));
                    sockList.push_back({s, targetIp});
                }

                if (sockList.empty()) continue;

                fd_set writeSet;
                FD_ZERO(&writeSet);
                for (const auto& item : sockList) {
                    FD_SET(item.first, &writeSet);
                }

                timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 250000; // 250ms timeout per batch

                int ready = select(0, nullptr, &writeSet, nullptr, &tv);
                if (ready > 0) {
                    for (const auto& item : sockList) {
                        if (FD_ISSET(item.first, &writeSet)) {
                            int sockErr = 0;
                            int errLen = sizeof(sockErr);
                            getsockopt(item.first, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sockErr), &errLen);
                            if (sockErr == 0) {
                                std::string foundIp = item.second;
                                std::thread([this, foundIp, scanPort]() {
                                    PingPeerHttp(foundIp, scanPort);
                                }).detach();
                            }
                        }
                    }
                }

                for (const auto& item : sockList) {
                    closesocket(item.first);
                }
            }
        }

        // Wait up to 4 seconds, waking early if m_scanRequested
        for (int i = 0; i < 40 && m_running; ++i) {
            if (m_scanRequested) {
                m_scanRequested = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
