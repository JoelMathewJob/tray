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

    bool SendFileToPeer(const std::string& peerIp, const std::wstring& filePath, std::string& outError);

private:
    NetworkManager() = default;
    ~NetworkManager();

    void UdpBroadcastLoop();
    void UdpListenLoop();
    void HttpServerLoop();

    std::atomic<bool> m_running{false};
    std::atomic<int> m_httpPort{8088};
    std::thread m_udpBroadcastThread;
    std::thread m_udpListenThread;
    std::thread m_httpServerThread;

    std::unique_ptr<httplib::Server> m_httpServer;

    std::string m_cachedLocalIp;
    ULONGLONG m_lastIpCheck = 0;
};
