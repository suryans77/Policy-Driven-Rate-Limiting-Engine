#include "Server.h"

#include "JsonCodec.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

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
    if (status == 404) return "Not Found";
    if (status == 500) return "Internal Server Error";
    return "OK";
}

bool parseHttpRequest(const std::string& raw, HttpRequest& request) {
    std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;

    std::istringstream headers(raw.substr(0, headerEnd));
    headers >> request.method >> request.path;

    std::string line;
    std::getline(headers, line);
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
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

std::string readRequest(SocketHandle client) {
    std::string raw;
    char buffer[4096];
    int expectedBody = -1;

    while (true) {
        int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        raw.append(buffer, buffer + received);

        std::size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos && expectedBody < 0) {
            std::string headers = raw.substr(0, headerEnd);
            std::size_t contentLength = headers.find("Content-Length:");
            if (contentLength != std::string::npos) {
                std::size_t start = contentLength + std::strlen("Content-Length:");
                while (start < headers.size() && headers[start] == ' ') ++start;
                expectedBody = std::atoi(headers.c_str() + start);
            } else {
                expectedBody = 0;
            }
        }

        if (expectedBody >= 0) {
            std::size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos
                && raw.size() >= headerEnd + 4 + static_cast<std::size_t>(expectedBody)) {
                break;
            }
        }
    }

    return raw;
}
}

Server::Server(RestController& controller, int port)
    : controller_(controller),
      port_(port)
{}

void Server::run() {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return;
    }
#endif

    SocketHandle serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET_HANDLE) {
        std::cerr << "Unable to create server socket\n";
        return;
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
        return;
    }

    if (listen(serverSocket, 16) < 0) {
        std::cerr << "Unable to listen on port " << port_ << "\n";
        closeSocket(serverSocket);
        return;
    }

    std::cout << "Rlimit REST API listening on http://0.0.0.0:" << port_ << "\n";
    while (true) {
        SocketHandle client = accept(serverSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET_HANDLE) continue;

        HttpRequest request;
        HttpResponse response;
        if (parseHttpRequest(readRequest(client), request)) {
            response = controller_.handleRequest(request);
        } else {
            response = HttpResponse::json(400, JsonCodec::encodeError("malformed HTTP request"));
        }

        std::string serialized = serializeHttpResponse(response);
        send(client, serialized.c_str(), static_cast<int>(serialized.size()), 0);
        closeSocket(client);
    }
}
