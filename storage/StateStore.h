#pragma once

#include <string>
#include <vector>

class StateStore {
public:
    virtual bool get(const std::string& key, std::string& value) = 0;
    virtual void set(const std::string& key, const std::string& value) = 0;
    virtual long long incr(const std::string& key) = 0;
    virtual void expire(const std::string& key, int seconds) = 0;

    virtual bool supportsAtomicScripts() const { return false; }
    virtual bool eval(const std::string& script,
                      const std::vector<std::string>& keys,
                      const std::vector<std::string>& args,
                      std::string& result,
                      std::string& error);

    virtual ~StateStore() = default;
};
