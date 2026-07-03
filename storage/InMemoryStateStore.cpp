#include "InMemoryStateStore.h"

#include <cstdlib>

void InMemoryStateStore::evictExpired(const std::string& key) {
    auto exp = expiry_.find(key);
    if (exp != expiry_.end() && std::chrono::steady_clock::now() >= exp->second) {
        values_.erase(key);
        expiry_.erase(exp);
    }
}

bool InMemoryStateStore::get(const std::string& key, std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    evictExpired(key);
    auto it = values_.find(key);
    if (it == values_.end()) return false;
    value = it->second;
    return true;
}

void InMemoryStateStore::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[key] = value;
}

long long InMemoryStateStore::incr(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    evictExpired(key);
    long long value = 0;
    auto it = values_.find(key);
    if (it != values_.end()) {
        value = std::strtoll(it->second.c_str(), nullptr, 10);
    }
    ++value;
    values_[key] = std::to_string(value);
    return value;
}

void InMemoryStateStore::expire(const std::string& key, int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    expiry_[key] = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}
