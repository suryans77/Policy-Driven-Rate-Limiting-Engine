#pragma once

#include "StateStore.h"

#include <string>
#include <vector>

class RedisStateStore : public StateStore {
public:
    struct Reply {
        enum Type { SimpleString, BulkString, Integer, Array, Nil, Error } type = Nil;
        std::string text;
        long long integer = 0;
        std::vector<Reply> items;
    };

    RedisStateStore(const std::string& host, int port);

    bool get(const std::string& key, std::string& value) override;
    void set(const std::string& key, const std::string& value) override;
    long long incr(const std::string& key) override;
    void expire(const std::string& key, int seconds) override;

    bool supportsAtomicScripts() const override;
    bool eval(const std::string& script,
              const std::vector<std::string>& keys,
              const std::vector<std::string>& args,
              std::string& result,
              std::string& error) override;

private:
    bool command(const std::vector<std::string>& args, Reply& reply, std::string& error);

    std::string host_;
    int port_;
};
