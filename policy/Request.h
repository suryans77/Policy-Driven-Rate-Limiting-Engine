#pragma once

#include <chrono>
#include <string>

struct Request {
    std::string tenantId;
    std::string endpoint;
    std::chrono::system_clock::time_point receivedAt;

    Request()
        : receivedAt(std::chrono::system_clock::now())
    {}

    Request(const std::string& tenant, const std::string& path)
        : tenantId(tenant),
          endpoint(path),
          receivedAt(std::chrono::system_clock::now())
    {}
};
