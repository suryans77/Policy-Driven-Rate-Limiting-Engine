#include "RedisStateStore.h"

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <mutex>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandle = SOCKET;
  const SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
  #include <fcntl.h>
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
        if (line.size() > 65536) return false;
    }
    return false;
}

bool readBytes(SocketHandle socket, std::size_t count, std::string& out) {
    if (count > 16 * 1024 * 1024) return false;
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

bool parseLongLong(const std::string& text, long long& value) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    value = parsed;
    return true;
}

bool parseReply(SocketHandle socket, RedisStateStore::Reply& reply,
                std::string& error, int depth = 0) {
    if (depth > 16) {
        error = "Redis response nesting limit exceeded";
        return false;
    }
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
        long long integer = 0;
        if (!parseLongLong(line, integer)) {
            error = "invalid Redis integer reply";
            return false;
        }
        reply.type = RedisStateStore::Reply::Integer;
        reply.integer = integer;
        return true;
    }
    if (type == '$') {
        if (!readLine(socket, line)) return false;
        long long len = 0;
        if (!parseLongLong(line, len) || len < -1) {
            error = "invalid Redis bulk length";
            return false;
        }
        if (len == -1) {
            reply.type = RedisStateStore::Reply::Nil;
            return true;
        }
        if (len > 16LL * 1024LL * 1024LL) {
            error = "Redis bulk response is too large";
            return false;
        }
        reply.type = RedisStateStore::Reply::BulkString;
        return readBytes(socket, static_cast<std::size_t>(len), reply.text);
    }
    if (type == '*') {
        if (!readLine(socket, line)) return false;
        long long count = 0;
        if (!parseLongLong(line, count) || count < -1) {
            error = "invalid Redis array length";
            return false;
        }
        if (count == -1) {
            reply.type = RedisStateStore::Reply::Nil;
            return true;
        }
        if (count > 1024) {
            error = "Redis response array is too large";
            return false;
        }
        reply.type = RedisStateStore::Reply::Array;
        reply.items.resize(static_cast<std::size_t>(count));
        for (long long i = 0; i < count; ++i) {
            if (!parseReply(socket, reply.items[static_cast<std::size_t>(i)], error, depth + 1)) return false;
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
    static std::once_flag winsockOnce;
    static int winsockResult = 0;
    std::call_once(winsockOnce, []() {
        WSADATA data;
        winsockResult = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (winsockResult != 0) {
        error = "WSAStartup failed";
        return INVALID_SOCKET_HANDLE;
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

#ifdef _WIN32
        u_long nonBlocking = 1;
        bool modeSet = ioctlsocket(candidate, FIONBIO, &nonBlocking) == 0;
#else
        int originalFlags = fcntl(candidate, F_GETFL, 0);
        bool modeSet = originalFlags >= 0
            && fcntl(candidate, F_SETFL, originalFlags | O_NONBLOCK) == 0;
#endif
        bool connectStarted = false;
        if (modeSet) {
            int connectResult = connect(candidate, rp->ai_addr,
                                        static_cast<int>(rp->ai_addrlen));
            if (connectResult == 0) {
                connectStarted = true;
            } else {
#ifdef _WIN32
                int socketError = WSAGetLastError();
                connectStarted = socketError == WSAEWOULDBLOCK
                    || socketError == WSAEINPROGRESS;
#else
                connectStarted = errno == EINPROGRESS || errno == EWOULDBLOCK;
#endif
            }
        }

        bool connectSucceeded = false;
        if (connectStarted) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(candidate, &writable);
            timeval timeout;
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
#ifdef _WIN32
            int selected = select(0, nullptr, &writable, nullptr, &timeout);
#else
            int selected = select(candidate + 1, nullptr,
                                  &writable, nullptr, &timeout);
#endif
            if (selected > 0 && FD_ISSET(candidate, &writable)) {
                int socketError = 0;
#ifdef _WIN32
                int length = sizeof(socketError);
                if (getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&socketError), &length) == 0
                    && socketError == 0) connectSucceeded = true;
#else
                socklen_t length = sizeof(socketError);
                if (getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                               &socketError, &length) == 0
                    && socketError == 0) connectSucceeded = true;
#endif
            }
        }

#ifdef _WIN32
        nonBlocking = 0;
        if (modeSet) ioctlsocket(candidate, FIONBIO, &nonBlocking);
#else
        if (modeSet) fcntl(candidate, F_SETFL, originalFlags);
#endif
        if (connectSucceeded) {
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

void setSocketTimeouts(SocketHandle socket) {
#ifdef _WIN32
    DWORD timeoutMs = 2000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}
}

RedisStateStore::RedisStateStore(const std::string& host, int port,
                                 const std::string& password)
    : host_(host),
      port_(port),
      password_(password)
{}

bool RedisStateStore::command(const std::vector<std::string>& args,
                              Reply& reply,
                              std::string& error) {
    // One persistent connection per calling worker avoids a global connection
    // lock while still reusing authenticated TCP sessions safely.
    thread_local std::unordered_map<const RedisStateStore*, SocketHandle> connections;
    SocketHandle& socket = connections[this];
    if (socket == 0 || socket == INVALID_SOCKET_HANDLE) {
        socket = connectSocket(host_, port_, error);
        if (socket == INVALID_SOCKET_HANDLE) return false;
        setSocketTimeouts(socket);

        if (!password_.empty()) {
            Reply authReply;
            if (!sendAll(socket, encodeCommand({"AUTH", password_}))
                || !parseReply(socket, authReply, error)
                || authReply.type != Reply::SimpleString
                || authReply.text != "OK") {
                closeSocket(socket);
                connections.erase(this);
                if (error.empty()) error = "Redis authentication failed";
                return false;
            }
        }
    }

    std::string encoded = encodeCommand(args);
    bool ok = sendAll(socket, encoded) && parseReply(socket, reply, error);
    if (!ok) {
        closeSocket(socket);
        connections.erase(this);
    }
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

bool RedisStateStore::health(std::string& error) {
    Reply reply;
    if (!command({"PING"}, reply, error)) return false;
    return reply.type == Reply::SimpleString && reply.text == "PONG";
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
