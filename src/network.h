#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <windows.h>
#include "state.h"

namespace httplib { class Server; }

class NetworkManager {
public:
    static NetworkManager& Instance();

    bool Start();
    void Stop();
    bool IsRunning() const { return m_running; }

    std::string GetLocalIP();
    int GetHttpPort() const { return m_httpPort.load(); }

    bool SendFileToPeer(const std::string& peerIp, const std::wstring& filePath, std::string& outError, int peerPort = 8088);
    void TriggerQuickScan();
    void PingPeerHttp(const std::string& peerIp, int peerPort);

private:
    NetworkManager() = default;
    ~NetworkManager();

    void UdpBroadcastLoop();
    void UdpListenLoop();
    void HttpServerLoop();
    void SubnetScanLoop();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_scanRequested{false};
    std::atomic<int> m_httpPort{8088};
    std::thread m_udpBroadcastThread;
    std::thread m_udpListenThread;
    std::thread m_httpServerThread;
    std::thread m_subnetScanThread;

    std::unique_ptr<httplib::Server> m_httpServer;

    std::string m_cachedLocalIp;
    ULONGLONG m_lastIpCheck = 0;
};
