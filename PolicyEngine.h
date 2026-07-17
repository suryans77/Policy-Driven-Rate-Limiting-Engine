#pragma once
#include "RateLimitStrategy.h"
#include <unordered_map>
#include <memory>
#include <string>
#include <mutex>
#include <shared_mutex>

struct EvaluationResult {
    bool found = false;
    bool allowed = false;
    std::string algorithm;
    std::string state;
    std::string error;
};

class PolicyEngine {
public:
    void setPolicy(const std::string& tenantId,
                   std::unique_ptr<RateLimitStrategy> strategy);
    bool ensurePolicy(const std::string& tenantId,
                      std::unique_ptr<RateLimitStrategy> strategy);

    // Thread-safe evaluate (mutex locked)
    bool evaluate(const std::string& tenantId);
    EvaluationResult evaluateDetailed(const std::string& tenantId);

    // Intentionally unsafe — no mutex, used ONLY to demonstrate the race condition
    bool evaluateUnsafe(const std::string& tenantId);

    std::string getState(const std::string& tenantId) const;
    bool hasTenant(const std::string& tenantId) const;
    std::string getAlgorithm(const std::string& tenantId) const;

    // Reset a tenant's strategy state (for stress test re-runs)
    void resetPolicy(const std::string& tenantId);

private:
    struct Entry {
        explicit Entry(std::unique_ptr<RateLimitStrategy> value)
            : strategy(std::move(value)) {}

        std::unique_ptr<RateLimitStrategy> strategy;
        mutable std::mutex mutex;
    };

    std::shared_ptr<Entry> findEntry(const std::string& tenantId) const;

    std::unordered_map<std::string, std::shared_ptr<Entry>> tenantPolicies_;
    mutable std::shared_mutex registryMutex_;
};
