#include "Server.h"

#include "JsonCodec.h"

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandle = SOCKET;
  const SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketHandle = int;
  const SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

namespace {
void closeSocket(SocketHandle socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

std::string statusText(int status) {
    if (status == 200) return "OK";
    if (status == 201) return "Created";
    if (status == 400) return "Bad Request";
    if (status == 401) return "Unauthorized";
    if (status == 403) return "Forbidden";
    if (status == 408) return "Request Timeout";
    if (status == 413) return "Payload Too Large";
    if (status == 415) return "Unsupported Media Type";
    if (status == 404) return "Not Found";
    if (status == 405) return "Method Not Allowed";
    if (status == 431) return "Request Header Fields Too Large";
    if (status == 500) return "Internal Server Error";
    if (status == 503) return "Service Unavailable";
    return "OK";
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size()
           && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    std::size_t last = value.size();
    while (last > first
           && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

bool parseHttpRequest(const std::string& raw, HttpRequest& request) {
    std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;

    std::istringstream headers(raw.substr(0, headerEnd));
    std::string requestLine;
    if (!std::getline(headers, requestLine)) return false;
    if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();
    std::istringstream requestLineStream(requestLine);
    std::string version;
    std::string extra;
    requestLineStream >> request.method >> request.path >> version;
    if (requestLineStream >> extra) return false;
    if ((version != "HTTP/1.1" && version != "HTTP/1.0")
        || request.method.empty() || request.path.empty()) return false;
    auto query = request.path.find('?');
    if (query != std::string::npos) request.path.resize(query);

    std::string line;
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto pos = line.find(':');
        if (pos == std::string::npos) return false;
        std::string key = lower(trim(line.substr(0, pos)));
        std::string value = trim(line.substr(pos + 1));
        if (key.empty() || request.headers.count(key) > 0) return false;
        request.headers[key] = value;
    }

    request.body = raw.substr(headerEnd + 4);
    return true;
}

std::string serializeHttpResponse(const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " " << statusText(response.status) << "\r\n";
    for (const auto& header : response.headers) {
        out << header.first << ": " << header.second << "\r\n";
    }
    out << "Content-Length: " << response.body.size() << "\r\n\r\n";
    out << response.body;
    return out.str();
}

bool parseExpectedBody(const std::string& headers, int& expectedBody,
                       std::string& error, int& errorStatus) {
    std::istringstream input(headers);
    std::string line;
    std::getline(input, line);
    bool sawLength = false;
    expectedBody = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto pos = line.find(':');
        if (pos == std::string::npos) {
            error = "malformed HTTP header";
            errorStatus = 400;
            return false;
        }
        std::string key = lower(trim(line.substr(0, pos)));
        std::string value = trim(line.substr(pos + 1));
        if (key == "transfer-encoding") {
            error = "transfer-encoding is not supported";
            errorStatus = 400;
            return false;
        }
        if (key == "content-length") {
            if (sawLength || value.empty()
                || value.find_first_not_of("0123456789") != std::string::npos) {
                error = "invalid content-length";
                errorStatus = 400;
                return false;
            }
            sawLength = true;
            unsigned long long parsed = std::strtoull(value.c_str(), nullptr, 10);
            if (parsed > 1024ULL * 1024ULL) {
                error = "request body exceeds 1 MiB";
                errorStatus = 413;
                return false;
            }
            expectedBody = static_cast<int>(parsed);
        }
    }
    return true;
}

bool readRequest(SocketHandle client, std::string& raw,
                 std::string& error, int& errorStatus) {
    raw.clear();
    char buffer[4096];
    int expectedBody = -1;
    constexpr std::size_t maxHeaderBytes = 16 * 1024;

    while (true) {
        int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            error = "request timed out or connection closed";
            errorStatus = 408;
            return false;
        }
        raw.append(buffer, buffer + received);

        std::size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos && expectedBody < 0) {
            if (headerEnd > maxHeaderBytes) {
                error = "request headers exceed 16 KiB";
                errorStatus = 431;
                return false;
            }
            if (!parseExpectedBody(raw.substr(0, headerEnd), expectedBody,
                                   error, errorStatus)) return false;
        } else if (headerEnd == std::string::npos && raw.size() > maxHeaderBytes) {
            error = "request headers exceed 16 KiB";
            errorStatus = 431;
            return false;
        }

        if (expectedBody >= 0) {
            std::size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos && expectedBody == 0
                && raw.size() > headerEnd + 4) {
                error = "request body requires content-length";
                errorStatus = 400;
                return false;
            }
            if (headerEnd != std::string::npos
                && raw.size() >= headerEnd + 4 + static_cast<std::size_t>(expectedBody)) {
                break;
            }
        }
    }

    return true;
}

bool sendAll(SocketHandle socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int count = send(socket, data.data() + sent,
                         static_cast<int>(data.size() - sent), 0);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void setClientTimeouts(SocketHandle socket) {
#ifdef _WIN32
    DWORD timeoutMs = 5000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}
}

Server::Server(RestController& controller, int port, int workers,
               int maxQueuedConnections)
    : controller_(controller),
      port_(port),
      workers_(std::max(1, workers)),
      maxQueuedConnections_(std::max(1, maxQueuedConnections))
{}

bool Server::run() {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return false;
    }
#endif

    SocketHandle serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET_HANDLE) {
        std::cerr << "Unable to create server socket\n";
        return false;
    }

    int yes = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(port_));

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "Unable to bind HTTP server on port " << port_ << "\n";
        closeSocket(serverSocket);
        return false;
    }

    if (listen(serverSocket, 16) < 0) {
        std::cerr << "Unable to listen on port " << port_ << "\n";
        closeSocket(serverSocket);
        return false;
    }

    std::queue<SocketHandle> pending;
    std::mutex pendingMutex;
    std::condition_variable pendingReady;

    auto processClient = [&]() {
        while (true) {
            SocketHandle client;
            {
                std::unique_lock<std::mutex> lock(pendingMutex);
                pendingReady.wait(lock, [&]() { return !pending.empty(); });
                client = pending.front();
                pending.pop();
            }

            HttpRequest request;
            HttpResponse response;
            std::string raw;
            std::string error;
            int errorStatus = 400;
            if (readRequest(client, raw, error, errorStatus)
                && parseHttpRequest(raw, request)) {
                response = controller_.handleRequest(request);
            } else {
                if (error.empty()) error = "malformed HTTP request";
                response = HttpResponse::json(errorStatus, JsonCodec::encodeError(error));
            }

            sendAll(client, serializeHttpResponse(response));
            closeSocket(client);
        }
    };

    std::vector<std::thread> workerThreads;
    workerThreads.reserve(static_cast<std::size_t>(workers_));
    for (int i = 0; i < workers_; ++i) workerThreads.emplace_back(processClient);

    std::cout << "Rlimit REST API listening on http://0.0.0.0:" << port_
              << " with " << workers_ << " workers\n";
    while (true) {
        SocketHandle client = accept(serverSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET_HANDLE) continue;
        setClientTimeouts(client);
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            if (static_cast<int>(pending.size()) < maxQueuedConnections_) {
                pending.push(client);
                queued = true;
            }
        }
        if (queued) {
            pendingReady.notify_one();
        } else {
            sendAll(client, serializeHttpResponse(HttpResponse::json(
                503, JsonCodec::encodeError("server is busy"))));
            closeSocket(client);
        }
    }
    return true;
}
