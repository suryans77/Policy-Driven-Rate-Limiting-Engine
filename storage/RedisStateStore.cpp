#include "RedisStateStore.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandle = SOCKET;
  const SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
  #include <netdb.h>
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

bool sendAll(SocketHandle socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int n = send(socket, data.c_str() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recvByte(SocketHandle socket, char& c) {
    return recv(socket, &c, 1, 0) == 1;
}

bool readLine(SocketHandle socket, std::string& line) {
    line.clear();
    char c = 0;
    while (recvByte(socket, c)) {
        if (c == '\r') {
            char next = 0;
            if (!recvByte(socket, next) || next != '\n') return false;
            return true;
        }
        line.push_back(c);
    }
    return false;
}

bool readBytes(SocketHandle socket, std::size_t count, std::string& out) {
    out.clear();
    out.resize(count);
    std::size_t received = 0;
    while (received < count) {
        int n = recv(socket, &out[received], static_cast<int>(count - received), 0);
        if (n <= 0) return false;
        received += static_cast<std::size_t>(n);
    }
    char cr = 0;
    char lf = 0;
    return recvByte(socket, cr) && recvByte(socket, lf) && cr == '\r' && lf == '\n';
}

bool parseReply(SocketHandle socket, RedisStateStore::Reply& reply, std::string& error) {
    char type = 0;
    if (!recvByte(socket, type)) {
        error = "no response from Redis";
        return false;
    }

    std::string line;
    if (type == '+') {
        if (!readLine(socket, line)) return false;
        reply.type = RedisStateStore::Reply::SimpleString;
        reply.text = line;
        return true;
    }
    if (type == '-') {
        if (!readLine(socket, line)) return false;
        reply.type = RedisStateStore::Reply::Error;
        reply.text = line;
        error = line;
        return false;
    }
    if (type == ':') {
        if (!readLine(socket, line)) return false;
        reply.type = RedisStateStore::Reply::Integer;
        reply.integer = std::strtoll(line.c_str(), nullptr, 10);
        return true;
    }
    if (type == '$') {
        if (!readLine(socket, line)) return false;
        long long len = std::strtoll(line.c_str(), nullptr, 10);
        if (len < 0) {
            reply.type = RedisStateStore::Reply::Nil;
            return true;
        }
        reply.type = RedisStateStore::Reply::BulkString;
        return readBytes(socket, static_cast<std::size_t>(len), reply.text);
    }
    if (type == '*') {
        if (!readLine(socket, line)) return false;
        long long count = std::strtoll(line.c_str(), nullptr, 10);
        if (count < 0) {
            reply.type = RedisStateStore::Reply::Nil;
            return true;
        }
        reply.type = RedisStateStore::Reply::Array;
        reply.items.resize(static_cast<std::size_t>(count));
        for (long long i = 0; i < count; ++i) {
            if (!parseReply(socket, reply.items[static_cast<std::size_t>(i)], error)) return false;
        }
        return true;
    }

    error = "unknown Redis response type";
    return false;
}

std::string encodeCommand(const std::vector<std::string>& args) {
    std::ostringstream out;
    out << "*" << args.size() << "\r\n";
    for (const auto& arg : args) {
        out << "$" << arg.size() << "\r\n" << arg << "\r\n";
    }
    return out.str();
}

bool replyToString(const RedisStateStore::Reply& reply, std::string& value) {
    if (reply.type == RedisStateStore::Reply::BulkString
        || reply.type == RedisStateStore::Reply::SimpleString) {
        value = reply.text;
        return true;
    }
    if (reply.type == RedisStateStore::Reply::Integer) {
        value = std::to_string(reply.integer);
        return true;
    }
    return false;
}

SocketHandle connectSocket(const std::string& host, int port, std::string& error) {
#ifdef _WIN32
    static bool winsockReady = false;
    if (!winsockReady) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            error = "WSAStartup failed";
            return INVALID_SOCKET_HANDLE;
        }
        winsockReady = true;
    }
#endif

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0) {
        error = "unable to resolve Redis host";
        return INVALID_SOCKET_HANDLE;
    }

    SocketHandle connected = INVALID_SOCKET_HANDLE;
    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        SocketHandle candidate = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (candidate == INVALID_SOCKET_HANDLE) continue;
        if (connect(candidate, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
            connected = candidate;
            break;
        }
        closeSocket(candidate);
    }

    freeaddrinfo(result);
    if (connected == INVALID_SOCKET_HANDLE) {
        error = "unable to connect to Redis";
    }
    return connected;
}
}

RedisStateStore::RedisStateStore(const std::string& host, int port)
    : host_(host),
      port_(port)
{}

bool RedisStateStore::command(const std::vector<std::string>& args,
                              Reply& reply,
                              std::string& error) {
    SocketHandle socket = connectSocket(host_, port_, error);
    if (socket == INVALID_SOCKET_HANDLE) return false;

    std::string encoded = encodeCommand(args);
    bool ok = sendAll(socket, encoded) && parseReply(socket, reply, error);
    closeSocket(socket);
    return ok;
}

bool RedisStateStore::get(const std::string& key, std::string& value) {
    Reply reply;
    std::string error;
    if (!command({"GET", key}, reply, error)) return false;
    return replyToString(reply, value);
}

void RedisStateStore::set(const std::string& key, const std::string& value) {
    Reply reply;
    std::string error;
    command({"SET", key, value}, reply, error);
}

long long RedisStateStore::incr(const std::string& key) {
    Reply reply;
    std::string error;
    if (!command({"INCR", key}, reply, error)) return 0;
    return reply.type == Reply::Integer ? reply.integer : 0;
}

void RedisStateStore::expire(const std::string& key, int seconds) {
    Reply reply;
    std::string error;
    command({"EXPIRE", key, std::to_string(seconds)}, reply, error);
}

bool RedisStateStore::supportsAtomicScripts() const {
    return true;
}

bool RedisStateStore::eval(const std::string& script,
                           const std::vector<std::string>& keys,
                           const std::vector<std::string>& args,
                           std::string& result,
                           std::string& error) {
    std::vector<std::string> commandArgs;
    commandArgs.push_back("EVAL");
    commandArgs.push_back(script);
    commandArgs.push_back(std::to_string(keys.size()));
    commandArgs.insert(commandArgs.end(), keys.begin(), keys.end());
    commandArgs.insert(commandArgs.end(), args.begin(), args.end());

    Reply reply;
    if (!command(commandArgs, reply, error)) return false;
    if (!replyToString(reply, result)) {
        error = "Redis script returned an unsupported reply type";
        return false;
    }
    return true;
}
