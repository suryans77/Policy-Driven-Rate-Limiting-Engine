#include "RedisRateLimitStrategies.h"

#include "policy/Policy.h"

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
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

std::string redisHashTag(const std::string& value) {
    unsigned long long hash = 1469598103934665603ULL;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << "{rl:" << std::hex << std::setw(16) << std::setfill('0') << hash << "}";
    return out.str();
}

class RedisScriptStrategy : public RateLimitStrategy {
public:
    RedisScriptStrategy(StateStore& store, const std::string& key)
        : store_(store),
          key_(redisHashTag(key)),
          lastState_("redis_state=not_evaluated")
    {}

    std::string getState() const override {
        return lastState_;
    }

    std::string lastError() const override {
        return lastError_;
    }

protected:
    bool runScript(const std::string& script,
                   const std::vector<std::string>& keys,
                   const std::vector<std::string>& args,
                   std::string& result) {
        std::string error;
        if (!store_.eval(script, keys, args, result, error)) {
            lastError_ = error;
            lastState_ = "redis_error=" + error;
            return false;
        }
        lastError_.clear();
        return true;
    }

    StateStore& store_;
    std::string key_;
    mutable std::string lastState_;
    mutable std::string lastError_;
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
            "local t=redis.call('TIME');"
            "local now=tonumber(t[1])*1000+math.floor(tonumber(t[2])/1000);"
            "local tokens=tonumber(redis.call('GET', tokensKey) or ARGV[1]);"
            "local last=tonumber(redis.call('GET', tsKey) or tostring(now));"
            "local elapsed=(now-last)/1000.0;"
            "tokens=math.min(capacity, tokens + elapsed * refill);"
            "local allowed=0;"
            "if tokens >= 1.0 then tokens=tokens-1.0; allowed=1; end;"
            "redis.call('SET', tokensKey, tostring(tokens));"
            "redis.call('SET', tsKey, tostring(now));"
            "if refill > 0 then "
            "local ttl=math.max(1, math.ceil((capacity / refill) * 2));"
            "redis.call('EXPIRE', tokensKey, ttl);"
            "redis.call('EXPIRE', tsKey, ttl);"
            "else redis.call('PERSIST', tokensKey); redis.call('PERSIST', tsKey); end;"
            "return tostring(allowed)..'|'..tostring(tokens)";

        std::string result;
        bool scriptOk = runScript(script,
            {"bucket:" + key_ + ":tokens", "bucket:" + key_ + ":ts"},
            {std::to_string(capacity_), std::to_string(refillRate_)},
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
        static const std::string script =
            "local t=redis.call('TIME');"
            "local now=tonumber(t[1])*1000+math.floor(tonumber(t[2])/1000);"
            "redis.call('SET',KEYS[1],ARGV[1]); redis.call('SET',KEYS[2],tostring(now));"
            "local rate=tonumber(ARGV[2]); if rate > 0 then "
            "local ttl=math.max(1,math.ceil((tonumber(ARGV[1])/rate)*2));"
            "redis.call('EXPIRE',KEYS[1],ttl); redis.call('EXPIRE',KEYS[2],ttl);"
            "else redis.call('PERSIST',KEYS[1]); redis.call('PERSIST',KEYS[2]); end; return '1'";
        std::string result;
        runScript(script, {"bucket:" + key_ + ":tokens", "bucket:" + key_ + ":ts"},
                  {std::to_string(capacity_), std::to_string(refillRate_)}, result);
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
            "local t=redis.call('TIME');"
            "local now=tonumber(t[1])*1000+math.floor(tonumber(t[2])/1000);"
            "local counter=tonumber(redis.call('GET', counterKey) or '0');"
            "local start=tonumber(redis.call('GET', startKey) or tostring(now));"
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
            {std::to_string(limit_), std::to_string(windowSecs_ * 1000.0)},
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
        static const std::string script =
            "local t=redis.call('TIME');"
            "local now=tonumber(t[1])*1000+math.floor(tonumber(t[2])/1000);"
            "redis.call('SET',KEYS[1],'0'); redis.call('SET',KEYS[2],tostring(now));"
            "local ttl=math.max(1,math.ceil(tonumber(ARGV[1])/1000.0)*2);"
            "redis.call('EXPIRE',KEYS[1],ttl); redis.call('EXPIRE',KEYS[2],ttl); return '1'";
        std::string result;
        runScript(script, {"fixed:" + key_ + ":counter", "fixed:" + key_ + ":start"},
                  {std::to_string(windowSecs_ * 1000.0)}, result);
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
            "local t=redis.call('TIME');"
            "local now=tonumber(t[1])*1000+math.floor(tonumber(t[2])/1000);"
            "local prev=tonumber(redis.call('GET', prevKey) or '0');"
            "local curr=tonumber(redis.call('GET', currKey) or '0');"
            "local start=tonumber(redis.call('GET', startKey) or tostring(now));"
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
            {std::to_string(limit_), std::to_string(windowSecs_ * 1000.0)},
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
        static const std::string script =
            "local t=redis.call('TIME');"
            "local now=tonumber(t[1])*1000+math.floor(tonumber(t[2])/1000);"
            "redis.call('SET',KEYS[1],'0'); redis.call('SET',KEYS[2],'0');"
            "redis.call('SET',KEYS[3],tostring(now));"
            "local ttl=math.max(1,math.ceil(tonumber(ARGV[1])/1000.0)*3);"
            "redis.call('EXPIRE',KEYS[1],ttl); redis.call('EXPIRE',KEYS[2],ttl);"
            "redis.call('EXPIRE',KEYS[3],ttl); return '1'";
        std::string result;
        runScript(script,
                  {"slidingctr:" + key_ + ":prev", "slidingctr:" + key_ + ":curr",
                   "slidingctr:" + key_ + ":start"},
                  {std::to_string(windowSecs_ * 1000.0)}, result);
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
            "local t=redis.call('TIME');"
            "local now=tonumber(t[1])*1000+math.floor(tonumber(t[2])/1000);"
            "local queue=tonumber(redis.call('GET', queueKey) or '0');"
            "local last=tonumber(redis.call('GET', tsKey) or tostring(now));"
            "local elapsed=(now-last)/1000.0;"
            "queue=math.max(0, queue - elapsed * leakRate);"
            "local allowed=0;"
            "if queue + 1.0 <= capacity then queue=queue+1.0; allowed=1; end;"
            "redis.call('SET', queueKey, tostring(queue));"
            "redis.call('SET', tsKey, tostring(now));"
            "if leakRate > 0 then "
            "local ttl=math.max(1, math.ceil((capacity / leakRate) * 2));"
            "redis.call('EXPIRE', queueKey, ttl);"
            "redis.call('EXPIRE', tsKey, ttl);"
            "else redis.call('PERSIST', queueKey); redis.call('PERSIST', tsKey); end;"
            "return tostring(allowed)..'|'..tostring(queue)";

        std::string result;
        bool scriptOk = runScript(script,
            {"leaky:" + key_ + ":queue", "leaky:" + key_ + ":ts"},
            {std::to_string(capacity_), std::to_string(leakRate_)},
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
        static const std::string script =
            "local t=redis.call('TIME');"
            "local now=tonumber(t[1])*1000+math.floor(tonumber(t[2])/1000);"
            "redis.call('SET',KEYS[1],'0'); redis.call('SET',KEYS[2],tostring(now));"
            "local rate=tonumber(ARGV[2]); if rate > 0 then "
            "local ttl=math.max(1,math.ceil((tonumber(ARGV[1])/rate)*2));"
            "redis.call('EXPIRE',KEYS[1],ttl); redis.call('EXPIRE',KEYS[2],ttl);"
            "else redis.call('PERSIST',KEYS[1]); redis.call('PERSIST',KEYS[2]); end; return '1'";
        std::string result;
        runScript(script, {"leaky:" + key_ + ":queue", "leaky:" + key_ + ":ts"},
                  {std::to_string(capacity_), std::to_string(leakRate_)}, result);
        lastState_ = "queue=0/" + std::to_string(capacity_);
    }

    std::string algorithmName() const override { return "LeakyBucket"; }

private:
    double capacity_;
    double leakRate_;
};

class RedisSlidingWindowLog : public RedisScriptStrategy {
public:
    RedisSlidingWindowLog(StateStore& store,
                          const std::string& key,
                          int limit,
                          double windowSecs)
        : RedisScriptStrategy(store, key),
          limit_(limit),
          windowSecs_(windowSecs)
    {}

    bool allowRequest() override {
        static const std::string script =
            "local logKey=KEYS[1];"
            "local seqKey=KEYS[2];"
            "local limit=tonumber(ARGV[1]);"
            "local windowMs=tonumber(ARGV[2]);"
            "local t=redis.call('TIME');"
            "local now=tonumber(t[1])*1000+math.floor(tonumber(t[2])/1000);"
            "redis.call('ZREMRANGEBYSCORE', logKey, '-inf', now-windowMs);"
            "local count=redis.call('ZCARD', logKey);"
            "local allowed=0;"
            "if count < limit then "
            "local seq=redis.call('INCR', seqKey);"
            "redis.call('ZADD', logKey, now, tostring(now)..'-'..tostring(seq));"
            "count=count+1; allowed=1; end;"
            "local ttl=math.max(1, math.ceil((windowMs/1000.0)*2));"
            "redis.call('EXPIRE', logKey, ttl); redis.call('EXPIRE', seqKey, ttl);"
            "return tostring(allowed)..'|'..tostring(count)";

        std::string result;
        if (!runScript(script,
                       {"slidinglog:" + key_ + ":entries", "slidinglog:" + key_ + ":seq"},
                       {std::to_string(limit_), std::to_string(windowSecs_ * 1000.0)},
                       result)) return false;

        auto parts = splitPipe(result);
        int count = parts.size() > 1 ? static_cast<int>(toDouble(parts[1])) : 0;
        std::ostringstream state;
        state << "log_size=" << count << "/" << limit_ << " window=" << windowSecs_ << "s";
        lastState_ = state.str();
        return !parts.empty() && parts[0] == "1";
    }

    void reset() override {
        std::string result;
        std::string error;
        store_.eval("redis.call('DEL', KEYS[1], KEYS[2]); return '1'",
                    {"slidinglog:" + key_ + ":entries", "slidinglog:" + key_ + ":seq"},
                    {}, result, error);
        lastError_ = error;
        lastState_ = "log_size=0/" + std::to_string(limit_);
    }

    std::string algorithmName() const override { return "SlidingWindowLog"; }

private:
    int limit_;
    double windowSecs_;
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
    if (normalized == "SlidingWindowLog") {
        if (!readParam(params, "limit", first, error)) return nullptr;
        if (!readParam(params, "window", second, error)) return nullptr;
        return std::make_unique<RedisSlidingWindowLog>(store, stateKey, static_cast<int>(first), second);
    }
    if (normalized == "LeakyBucket") {
        if (!readParam(params, "capacity", first, error)) return nullptr;
        if (!readParam(params, "leakRate", second, error)) return nullptr;
        return std::make_unique<RedisLeakyBucket>(store, stateKey, first, second);
    }

    if (error) *error = "Redis state is not implemented for " + normalized;
    return nullptr;
}
