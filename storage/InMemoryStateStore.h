#pragma once

#include "StateStore.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

class InMemoryStateStore : public StateStore {
public:
    bool get(const std::string& key, std::string& value) override;
    void set(const std::string& key, const std::string& value) override;
    long long incr(const std::string& key) override;
    void expire(const std::string& key, int seconds) override;

private:
    void evictExpired(const std::string& key);

    std::unordered_map<std::string, std::string> values_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> expiry_;
    std::mutex mutex_;
};
