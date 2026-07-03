#include "RedisRateLimitStrategies.h"

#include "policy/Policy.h"

#include <chrono>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace {
long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool readParam(const std::map<std::string, double>& params,
               const std::string& name,
               double& value,
               std::string* error) {
    auto it = params.find(name);
    if (it == params.end()) {
        if (error) *error = "missing parameter: " + name;
        return false;
    }
    value = it->second;
    return true;
}

std::vector<std::string> splitPipe(const std::string& value) {
    std::vector<std::string> parts;
    std::stringstream ss(value);
    std::string part;
    while (std::getline(ss, part, '|')) {
        parts.push_back(part);
    }
    return parts;
}

double toDouble(const std::string& value) {
    return std::strtod(value.c_str(), nullptr);
}

class RedisScriptStrategy : public RateLimitStrategy {
public:
    RedisScriptStrategy(StateStore& store, const std::string& key)
        : store_(store),
          key_(key),
          lastState_("redis_state=not_evaluated")
    {}

    std::string getState() const override {
        return lastState_;
    }

protected:
    bool runScript(const std::string& script,
                   const std::vector<std::string>& keys,
                   const std::vector<std::string>& args,
                   std::string& result) {
        std::string error;
        if (!store_.eval(script, keys, args, result, error)) {
            lastState_ = "redis_error=" + error;
            return false;
        }
        return true;
    }

    StateStore& store_;
    std::string key_;
    mutable std::string lastState_;
};

class RedisTokenBucket : public RedisScriptStrategy {
public:
    RedisTokenBucket(StateStore& store,
                     const std::string& key,
                     double capacity,
                     double refillRate)
        : RedisScriptStrategy(store, key),
          capacity_(capacity),
          refillRate_(refillRate)
    {}

    bool allowRequest() override {
        static const std::string script =
            "local tokensKey=KEYS[1];"
            "local tsKey=KEYS[2];"
            "local capacity=tonumber(ARGV[1]);"
            "local refill=tonumber(ARGV[2]);"
            "local now=tonumber(ARGV[3]);"
            "local tokens=tonumber(redis.call('GET', tokensKey) or ARGV[1]);"
            "local last=tonumber(redis.call('GET', tsKey) or ARGV[3]);"
            "local elapsed=(now-last)/1000.0;"
            "tokens=math.min(capacity, tokens + elapsed * refill);"
            "local allowed=0;"
            "if tokens >= 1.0 then tokens=tokens-1.0; allowed=1; end;"
            "redis.call('SET', tokensKey, tostring(tokens));"
            "redis.call('SET', tsKey, tostring(now));"
            "local ttl=math.max(1, math.ceil(capacity / math.max(refill, 1)) * 2);"
            "redis.call('EXPIRE', tokensKey, ttl);"
            "redis.call('EXPIRE', tsKey, ttl);"
            "return tostring(allowed)..'|'..tostring(tokens)";

        std::string result;
        bool scriptOk = runScript(script,
            {"bucket:" + key_ + ":tokens", "bucket:" + key_ + ":ts"},
            {std::to_string(capacity_), std::to_string(refillRate_), std::to_string(nowMs())},
            result);
        if (!scriptOk) return false;

        auto parts = splitPipe(result);
        double tokens = parts.size() > 1 ? toDouble(parts[1]) : 0.0;
        std::ostringstream state;
        state << "tokens=" << tokens << "/" << capacity_ << " refill=" << refillRate_ << "/s";
        lastState_ = state.str();
        return !parts.empty() && parts[0] == "1";
    }

    void reset() override {
        store_.set("bucket:" + key_ + ":tokens", std::to_string(capacity_));
        store_.set("bucket:" + key_ + ":ts", std::to_string(nowMs()));
        lastState_ = "tokens=" + std::to_string(capacity_) + "/" + std::to_string(capacity_);
    }

    std::string algorithmName() const override { return "TokenBucket"; }

private:
    double capacity_;
    double refillRate_;
};

class RedisFixedWindowCounter : public RedisScriptStrategy {
public:
    RedisFixedWindowCounter(StateStore& store,
                            const std::string& key,
                            int limit,
                            double windowSecs)
        : RedisScriptStrategy(store, key),
          limit_(limit),
          windowSecs_(windowSecs)
    {}

    bool allowRequest() override {
        static const std::string script =
            "local counterKey=KEYS[1];"
            "local startKey=KEYS[2];"
            "local limit=tonumber(ARGV[1]);"
            "local windowMs=tonumber(ARGV[2]);"
            "local now=tonumber(ARGV[3]);"
            "local counter=tonumber(redis.call('GET', counterKey) or '0');"
            "local start=tonumber(redis.call('GET', startKey) or ARGV[3]);"
            "if now - start >= windowMs then counter=0; start=now; end;"
            "local allowed=0;"
            "if counter < limit then counter=counter+1; allowed=1; end;"
            "redis.call('SET', counterKey, tostring(counter));"
            "redis.call('SET', startKey, tostring(start));"
            "local ttl=math.max(1, math.ceil(windowMs / 1000.0) * 2);"
            "redis.call('EXPIRE', counterKey, ttl);"
            "redis.call('EXPIRE', startKey, ttl);"
            "local remaining=math.max(0, windowMs - (now - start)) / 1000.0;"
            "return tostring(allowed)..'|'..tostring(counter)..'|'..tostring(remaining)";

        std::string result;
        bool scriptOk = runScript(script,
            {"fixed:" + key_ + ":counter", "fixed:" + key_ + ":start"},
            {std::to_string(limit_), std::to_string(windowSecs_ * 1000.0), std::to_string(nowMs())},
            result);
        if (!scriptOk) return false;

        auto parts = splitPipe(result);
        double counter = parts.size() > 1 ? toDouble(parts[1]) : 0.0;
        double remaining = parts.size() > 2 ? toDouble(parts[2]) : 0.0;
        std::ostringstream state;
        state << "count=" << static_cast<int>(counter) << "/" << limit_
              << " window_remaining=" << remaining << "s"
              << " window=" << windowSecs_ << "s";
        lastState_ = state.str();
        return !parts.empty() && parts[0] == "1";
    }

    void reset() override {
        store_.set("fixed:" + key_ + ":counter", "0");
        store_.set("fixed:" + key_ + ":start", std::to_string(nowMs()));
        lastState_ = "count=0/" + std::to_string(limit_);
    }

    std::string algorithmName() const override { return "FixedWindowCounter"; }

private:
    int limit_;
    double windowSecs_;
};

class RedisSlidingWindowCounter : public RedisScriptStrategy {
public:
    RedisSlidingWindowCounter(StateStore& store,
                              const std::string& key,
                              int limit,
                              double windowSecs)
        : RedisScriptStrategy(store, key),
          limit_(limit),
          windowSecs_(windowSecs)
    {}

    bool allowRequest() override {
        static const std::string script =
            "local prevKey=KEYS[1];"
            "local currKey=KEYS[2];"
            "local startKey=KEYS[3];"
            "local limit=tonumber(ARGV[1]);"
            "local windowMs=tonumber(ARGV[2]);"
            "local now=tonumber(ARGV[3]);"
            "local prev=tonumber(redis.call('GET', prevKey) or '0');"
            "local curr=tonumber(redis.call('GET', currKey) or '0');"
            "local start=tonumber(redis.call('GET', startKey) or ARGV[3]);"
            "local elapsed=now-start;"
            "if elapsed >= windowMs then "
            "local windows=math.floor(elapsed / windowMs);"
            "if windows == 1 then prev=curr; else prev=0; end;"
            "curr=0; start=start + windows * windowMs; elapsed=now-start;"
            "end;"
            "local f=elapsed / windowMs;"
            "local estimate=prev * (1.0 - f) + curr;"
            "local allowed=0;"
            "if estimate < limit then curr=curr+1; allowed=1; end;"
            "redis.call('SET', prevKey, tostring(prev));"
            "redis.call('SET', currKey, tostring(curr));"
            "redis.call('SET', startKey, tostring(start));"
            "local ttl=math.max(1, math.ceil(windowMs / 1000.0) * 3);"
            "redis.call('EXPIRE', prevKey, ttl);"
            "redis.call('EXPIRE', currKey, ttl);"
            "redis.call('EXPIRE', startKey, ttl);"
            "return tostring(allowed)..'|'..tostring(prev)..'|'..tostring(curr)..'|'..tostring(f)..'|'..tostring(estimate)";

        std::string result;
        bool scriptOk = runScript(script,
            {"slidingctr:" + key_ + ":prev", "slidingctr:" + key_ + ":curr", "slidingctr:" + key_ + ":start"},
            {std::to_string(limit_), std::to_string(windowSecs_ * 1000.0), std::to_string(nowMs())},
            result);
        if (!scriptOk) return false;

        auto parts = splitPipe(result);
        int prev = parts.size() > 1 ? static_cast<int>(toDouble(parts[1])) : 0;
        int curr = parts.size() > 2 ? static_cast<int>(toDouble(parts[2])) : 0;
        double f = parts.size() > 3 ? toDouble(parts[3]) : 0.0;
        double estimate = parts.size() > 4 ? toDouble(parts[4]) : 0.0;
        std::ostringstream state;
        state << "prev=" << prev << " curr=" << curr
              << " f=" << f << " estimate=" << estimate
              << "/" << limit_ << " window=" << windowSecs_ << "s";
        lastState_ = state.str();
        return !parts.empty() && parts[0] == "1";
    }

    void reset() override {
        store_.set("slidingctr:" + key_ + ":prev", "0");
        store_.set("slidingctr:" + key_ + ":curr", "0");
        store_.set("slidingctr:" + key_ + ":start", std::to_string(nowMs()));
        lastState_ = "prev=0 curr=0 estimate=0/" + std::to_string(limit_);
    }

    std::string algorithmName() const override { return "SlidingWindowCounter"; }

private:
    int limit_;
    double windowSecs_;
};

class RedisLeakyBucket : public RedisScriptStrategy {
public:
    RedisLeakyBucket(StateStore& store,
                     const std::string& key,
                     double capacity,
                     double leakRate)
        : RedisScriptStrategy(store, key),
          capacity_(capacity),
          leakRate_(leakRate)
    {}

    bool allowRequest() override {
        static const std::string script =
            "local queueKey=KEYS[1];"
            "local tsKey=KEYS[2];"
            "local capacity=tonumber(ARGV[1]);"
            "local leakRate=tonumber(ARGV[2]);"
            "local now=tonumber(ARGV[3]);"
            "local queue=tonumber(redis.call('GET', queueKey) or '0');"
            "local last=tonumber(redis.call('GET', tsKey) or ARGV[3]);"
            "local elapsed=(now-last)/1000.0;"
            "queue=math.max(0, queue - elapsed * leakRate);"
            "local allowed=0;"
            "if queue + 1.0 <= capacity then queue=queue+1.0; allowed=1; end;"
            "redis.call('SET', queueKey, tostring(queue));"
            "redis.call('SET', tsKey, tostring(now));"
            "local ttl=math.max(1, math.ceil(capacity / math.max(leakRate, 1)) * 2);"
            "redis.call('EXPIRE', queueKey, ttl);"
            "redis.call('EXPIRE', tsKey, ttl);"
            "return tostring(allowed)..'|'..tostring(queue)";

        std::string result;
        bool scriptOk = runScript(script,
            {"leaky:" + key_ + ":queue", "leaky:" + key_ + ":ts"},
            {std::to_string(capacity_), std::to_string(leakRate_), std::to_string(nowMs())},
            result);
        if (!scriptOk) return false;

        auto parts = splitPipe(result);
        double queue = parts.size() > 1 ? toDouble(parts[1]) : 0.0;
        std::ostringstream state;
        state << "queue=" << queue << "/" << capacity_ << " leak=" << leakRate_ << "/s";
        lastState_ = state.str();
        return !parts.empty() && parts[0] == "1";
    }

    void reset() override {
        store_.set("leaky:" + key_ + ":queue", "0");
        store_.set("leaky:" + key_ + ":ts", std::to_string(nowMs()));
        lastState_ = "queue=0/" + std::to_string(capacity_);
    }

    std::string algorithmName() const override { return "LeakyBucket"; }

private:
    double capacity_;
    double leakRate_;
};
}

std::unique_ptr<RateLimitStrategy> createRedisBackedStrategy(
    const std::string& algorithm,
    const std::map<std::string, double>& params,
    StateStore& store,
    const std::string& stateKey,
    std::string* error) {
    std::string normalized = normalizeAlgorithmName(algorithm);
    double first = 0.0;
    double second = 0.0;

    if (normalized == "TokenBucket") {
        if (!readParam(params, "capacity", first, error)) return nullptr;
        if (!readParam(params, "refill", second, error)) return nullptr;
        return std::make_unique<RedisTokenBucket>(store, stateKey, first, second);
    }
    if (normalized == "FixedWindowCounter") {
        if (!readParam(params, "limit", first, error)) return nullptr;
        if (!readParam(params, "window", second, error)) return nullptr;
        return std::make_unique<RedisFixedWindowCounter>(store, stateKey, static_cast<int>(first), second);
    }
    if (normalized == "SlidingWindowCounter") {
        if (!readParam(params, "limit", first, error)) return nullptr;
        if (!readParam(params, "window", second, error)) return nullptr;
        return std::make_unique<RedisSlidingWindowCounter>(store, stateKey, static_cast<int>(first), second);
    }
    if (normalized == "LeakyBucket") {
        if (!readParam(params, "capacity", first, error)) return nullptr;
        if (!readParam(params, "leakRate", second, error)) return nullptr;
        return std::make_unique<RedisLeakyBucket>(store, stateKey, first, second);
    }

    if (error) *error = "Redis state is not implemented for " + normalized;
    return nullptr;
}
