#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <iostream>

#pragma comment(lib, "Ws2_32.lib")

namespace httplib {

using Headers = std::map<std::string, std::string>;

struct MultipartFormData {
    std::string name;
    std::string content;
    std::string filename;
    std::string content_type;
};

using MultipartFormDataItems = std::vector<MultipartFormData>;

struct Request {
    std::string method;
    std::string path;
    std::string body;
    std::string remote_addr;
    int remote_port = 0;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> params;
    std::map<std::string, MultipartFormData> files;
    std::vector<MultipartFormData> file_list;

    bool has_file(const std::string& key) const {
        return files.find(key) != files.end();
    }

    const MultipartFormData& get_file_value(const std::string& key) const {
        static const MultipartFormData s_empty;
        auto it = files.find(key);
        if (it != files.end()) return it->second;
        return s_empty;
    }

    bool has_param(const std::string& key) const {
        return params.find(key) != params.end();
    }

    std::string get_param_value(const std::string& key) const {
        auto it = params.find(key);
        if (it != params.end()) return it->second;
        return "";
    }

    bool has_header(const std::string& key) const {
        return headers.find(key) != headers.end();
    }

    std::string get_header_value(const std::string& key) const {
        auto it = headers.find(key);
        if (it != headers.end()) return it->second;
        return "";
    }
};

struct Response {
    int status = 200;
    std::string body;
    std::map<std::string, std::string> headers;

    void set_content(const std::string& s, const char* content_type) {
        body = s;
        headers["Content-Type"] = content_type ? content_type : "text/plain";
        headers["Content-Length"] = std::to_string(body.size());
    }

    void set_header(const std::string& key, const std::string& val) {
        headers[key] = val;
    }
};

using Handler = std::function<void(const Request&, Response&)>;

class Server {
private:
    std::map<std::string, Handler> m_getHandlers;
    std::map<std::string, Handler> m_postHandlers;
    std::atomic<bool> m_running{false};
    SOCKET m_listenSock = INVALID_SOCKET;
    std::mutex m_mutex;

public:
    Server() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }

    ~Server() {
        stop();
        WSACleanup();
    }

    void Get(const std::string& pattern, Handler handler) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_getHandlers[pattern] = handler;
    }

    void Post(const std::string& pattern, Handler handler) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_postHandlers[pattern] = handler;
    }

    void stop() {
        m_running = false;
        if (m_listenSock != INVALID_SOCKET) {
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
        }
    }

    int bind_to_any_port(const char* host, const std::vector<int>& candidate_ports) {
        for (int p : candidate_ports) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) continue;

            int opt = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<u_short>(p));
            if (!host || strcmp(host, "0.0.0.0") == 0) {
                addr.sin_addr.s_addr = INADDR_ANY;
            } else {
                inet_pton(AF_INET, host, &addr.sin_addr);
            }

            if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
                if (::listen(s, SOMAXCONN) != SOCKET_ERROR) {
                    sockaddr_in boundAddr = {};
                    int bLen = sizeof(boundAddr);
                    getsockname(s, reinterpret_cast<sockaddr*>(&boundAddr), &bLen);
                    int actualPort = ntohs(boundAddr.sin_port);
                    m_listenSock = s;
                    return actualPort;
                }
            }
            closesocket(s);
        }
        return -1;
    }

    bool listen_after_bind() {
        if (m_listenSock == INVALID_SOCKET) return false;
        m_running = true;

        while (m_running) {
            sockaddr_in clientAddr = {};
            int clientLen = sizeof(clientAddr);
            SOCKET clientSock = accept(m_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientSock == INVALID_SOCKET) {
                if (!m_running) break;
                continue;
            }

            char ipStr[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
            int clientPort = ntohs(clientAddr.sin_port);

            std::thread([this, clientSock, ipStr, clientPort]() {
                handleConnection(clientSock, ipStr, clientPort);
                closesocket(clientSock);
            }).detach();
        }

        return true;
    }

    bool listen(const char* host, int port) {
        std::vector<int> candidates = {port, 8088, 8090, 8880, 8000, 58080};
        int actual = bind_to_any_port(host, candidates);
        if (actual <= 0) return false;
        return listen_after_bind();
    }

private:
    void parseMultipart(Request& req, const std::string& boundary) {
        std::string delim = "--" + boundary;
        size_t pos = 0;
        while ((pos = req.body.find(delim, pos)) != std::string::npos) {
            pos += delim.size();
            if (req.body.substr(pos, 2) == "--") break; // end
            if (req.body.substr(pos, 2) == "\r\n") pos += 2;

            size_t headEnd = req.body.find("\r\n\r\n", pos);
            if (headEnd == std::string::npos) break;

            std::string partHeaders = req.body.substr(pos, headEnd - pos);
            pos = headEnd + 4;

            size_t nextDelim = req.body.find(delim, pos);
            if (nextDelim == std::string::npos) break;

            size_t bodyLen = nextDelim - pos;
            if (bodyLen >= 2 && req.body.substr(pos + bodyLen - 2, 2) == "\r\n")
                bodyLen -= 2;

            std::string partBody = req.body.substr(pos, bodyLen);

            MultipartFormData file;
            file.content = std::move(partBody);

            // Parse headers like: Content-Disposition: form-data; name="file"; filename="abc.txt"
            size_t namePos = partHeaders.find("name=\"");
            if (namePos != std::string::npos) {
                namePos += 6;
                size_t nameEnd = partHeaders.find("\"", namePos);
                if (nameEnd != std::string::npos)
                    file.name = partHeaders.substr(namePos, nameEnd - namePos);
            }
            size_t fnPos = partHeaders.find("filename=\"");
            if (fnPos != std::string::npos) {
                fnPos += 10;
                size_t fnEnd = partHeaders.find("\"", fnPos);
                if (fnEnd != std::string::npos)
                    file.filename = partHeaders.substr(fnPos, fnEnd - fnPos);
            }
            size_t ctPos = partHeaders.find("Content-Type: ");
            if (ctPos != std::string::npos) {
                ctPos += 14;
                size_t ctEnd = partHeaders.find("\r\n", ctPos);
                if (ctEnd != std::string::npos)
                    file.content_type = partHeaders.substr(ctPos, ctEnd - ctPos);
            }

            if (!file.name.empty() || !file.filename.empty()) {
                req.files[file.name] = file;
                req.file_list.push_back(file);
            }
            pos = nextDelim;
        }
    }

    void handleConnection(SOCKET sock, const std::string& remoteIp, int remotePort) {
        // Set generous receive timeout (60s) for multi-file and large payload uploads
        DWORD tv = 60000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

        std::string raw;
        char buf[65536];
        size_t contentLength = 0;
        bool headerDone = false;

        while (true) {
            int n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, n);

            if (!headerDone) {
                size_t hdrEnd = raw.find("\r\n\r\n");
                if (hdrEnd != std::string::npos) {
                    headerDone = true;
                    // look for Content-Length
                    std::string lower = raw.substr(0, hdrEnd);
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    size_t clPos = lower.find("content-length:");
                    if (clPos != std::string::npos) {
                        size_t valStart = clPos + 15;
                        size_t valEnd = lower.find("\r\n", valStart);
                        contentLength = std::strtoul(lower.substr(valStart, valEnd - valStart).c_str(), nullptr, 10);
                    }
                }
            }

            if (headerDone) {
                size_t hdrEnd = raw.find("\r\n\r\n");
                if (raw.size() - (hdrEnd + 4) >= contentLength) {
                    break; // got full body
                }
            }
        }

        if (raw.empty()) return;

        size_t hdrEnd = raw.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) return;

        std::string headerPart = raw.substr(0, hdrEnd);
        std::string bodyPart = raw.substr(hdrEnd + 4);

        Request req;
        req.remote_addr = remoteIp;
        req.remote_port = remotePort;
        req.body = std::move(bodyPart);

        // Parse Request Line
        std::istringstream stream(headerPart);
        std::string reqLine;
        if (std::getline(stream, reqLine)) {
            if (!reqLine.empty() && reqLine.back() == '\r') reqLine.pop_back();
            std::istringstream lineStream(reqLine);
            lineStream >> req.method >> req.path;
            if (req.path.empty() || req.path == "/index.html") req.path = "/";
        }

        // Query parameters
        size_t qPos = req.path.find('?');
        if (qPos != std::string::npos) {
            std::string qStr = req.path.substr(qPos + 1);
            req.path = req.path.substr(0, qPos);
            std::istringstream qs(qStr);
            std::string pair;
            while (std::getline(qs, pair, '&')) {
                size_t eq = pair.find('=');
                if (eq != std::string::npos)
                    req.params[pair.substr(0, eq)] = pair.substr(eq + 1);
                else
                    req.params[pair] = "";
            }
        }

        // Parse Headers
        std::string hLine;
        while (std::getline(stream, hLine)) {
            if (!hLine.empty() && hLine.back() == '\r') hLine.pop_back();
            size_t cPos = hLine.find(':');
            if (cPos != std::string::npos) {
                std::string k = hLine.substr(0, cPos);
                std::string v = hLine.substr(cPos + 1);
                while (!v.empty() && v.front() == ' ') v.erase(0, 1);
                req.headers[k] = v;
            }
        }

        // Check multipart
        auto ctIt = req.headers.find("Content-Type");
        if (ctIt == req.headers.end()) {
            for (const auto& h : req.headers) {
                std::string lowerKey = h.first;
                std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
                if (lowerKey == "content-type") {
                    ctIt = req.headers.find(h.first);
                    break;
                }
            }
        }

        if (ctIt != req.headers.end() && ctIt->second.find("multipart/form-data") != std::string::npos) {
            size_t bPos = ctIt->second.find("boundary=");
            if (bPos != std::string::npos) {
                std::string boundary = ctIt->second.substr(bPos + 9);
                parseMultipart(req, boundary);
            }
        }

        Response res;
        res.headers["Server"] = "WinTray-AirDrop/1.0";
        res.headers["Access-Control-Allow-Origin"] = "*";

        bool handled = false;
        std::unique_lock<std::mutex> lock(m_mutex);
        if (req.method == "GET") {
            auto it = m_getHandlers.find(req.path);
            if (it != m_getHandlers.end()) {
                auto fn = it->second;
                lock.unlock();
                fn(req, res);
                handled = true;
            }
        } else if (req.method == "POST") {
            auto it = m_postHandlers.find(req.path);
            if (it != m_postHandlers.end()) {
                auto fn = it->second;
                lock.unlock();
                fn(req, res);
                handled = true;
            }
        }

        if (!handled) {
            if (lock.owns_lock()) lock.unlock();
            res.status = 404;
            res.set_content("Not Found", "text/plain");
        }

        std::string statusMsg = (res.status == 200) ? "OK" : (res.status == 404) ? "Not Found" : "Error";
        std::string responseStr = "HTTP/1.1 " + std::to_string(res.status) + " " + statusMsg + "\r\n";
        for (const auto& h : res.headers) {
            responseStr += h.first + ": " + h.second + "\r\n";
        }
        responseStr += "\r\n" + res.body;

        send(sock, responseStr.data(), static_cast<int>(responseStr.size()), 0);
    }
};

struct Result {
    int status = 0;
    std::string body;

    bool operator!() const { return status == 0; }
    explicit operator bool() const { return status != 0; }
};

class Client {
private:
    std::string m_host;
    int m_port = 80;
    int m_timeoutSec = 5;

public:
    Client(const std::string& host, int port) : m_host(host), m_port(port) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }

    ~Client() {
        WSACleanup();
    }

    void set_connection_timeout(int sec, int = 0) { m_timeoutSec = sec; }
    void set_read_timeout(int sec, int = 0) { m_timeoutSec = sec; }

    Result Post(const std::string& path, const std::string& body, const char* content_type) {
        std::map<std::string, std::string> headers;
        headers["Content-Type"] = content_type ? content_type : "application/octet-stream";
        return sendRequest("POST", path, headers, body);
    }

    Result Post(const std::string& path, const MultipartFormDataItems& items) {
        std::string boundary = "----WinTrayBoundary" + std::to_string(GetTickCount64());
        std::string body;
        for (const auto& item : items) {
            body += "--" + boundary + "\r\n";
            body += "Content-Disposition: form-data; name=\"" + item.name + "\"";
            if (!item.filename.empty())
                body += "; filename=\"" + item.filename + "\"";
            body += "\r\n";
            if (!item.content_type.empty())
                body += "Content-Type: " + item.content_type + "\r\n";
            body += "\r\n";
            body += item.content + "\r\n";
        }
        body += "--" + boundary + "--\r\n";

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;
        return sendRequest("POST", path, headers, body);
    }

    Result Get(const std::string& path) {
        std::map<std::string, std::string> headers;
        return sendRequest("GET", path, headers, "");
    }

    Result Get(const std::string& path, const Headers& headers) {
        return sendRequest("GET", path, headers, "");
    }

private:
    Result sendRequest(const std::string& method, const std::string& path,
                        const std::map<std::string, std::string>& headers, const std::string& body) {
        Result result;
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return result;

        DWORD tv = m_timeoutSec * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(m_port));
        inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(sock);
            return result;
        }

        std::string reqStr = method + " " + path + " HTTP/1.1\r\n";
        reqStr += "Host: " + m_host + ":" + std::to_string(m_port) + "\r\n";
        reqStr += "Connection: close\r\n";
        reqStr += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        for (const auto& h : headers) {
            reqStr += h.first + ": " + h.second + "\r\n";
        }
        reqStr += "\r\n" + body;

        send(sock, reqStr.data(), static_cast<int>(reqStr.size()), 0);

        std::string rawResp;
        char buf[8192];
        while (true) {
            int n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            rawResp.append(buf, n);
        }
        closesocket(sock);

        size_t hdrEnd = rawResp.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) {
            std::string hdr = rawResp.substr(0, hdrEnd);
            result.body = rawResp.substr(hdrEnd + 4);

            size_t sp1 = hdr.find(' ');
            if (sp1 != std::string::npos) {
                size_t sp2 = hdr.find(' ', sp1 + 1);
                if (sp2 != std::string::npos) {
                    result.status = std::stoi(hdr.substr(sp1 + 1, sp2 - sp1 - 1));
                }
            }
        }

        return result;
    }
};

} // namespace httplib
