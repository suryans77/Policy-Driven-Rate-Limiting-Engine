#pragma once

#include <string>
#include <vector>
#include <mutex>

struct TenantRecord {
    std::string id;
    std::string name;
    std::string algorithm;
    std::string params;
};

struct RequestRecord {
    long long timestampMs;
    std::string result;
    std::string algorithm;
    std::string stateSnapshot;
};

class Database {
public:
    explicit Database(const std::string& path = "rlimit_store.txt");
    ~Database() = default;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool isAvailable() const;

    bool saveTenant(const std::string& id, const std::string& name,
                    const std::string& algorithm, const std::string& params);
    bool deleteTenant(const std::string& id);
    std::vector<TenantRecord> loadAllTenants();

    bool logRequest(const std::string& tenantId, long long timestampMs,
                    const std::string& result, const std::string& algorithm,
                    const std::string& stateSnapshot);

    std::vector<RequestRecord> getRequestLog(const std::string& tenantId, int limit = 100);
    int getTotalRequests(const std::string& tenantId);
    int getTotalAllowed(const std::string& tenantId);
    int getTotalDenied(const std::string& tenantId);

private:
    struct StoredRequest {
        std::string tenantId;
        RequestRecord record;
    };

    std::string path_;
    bool available_;

    std::vector<TenantRecord> tenants_;
    std::vector<StoredRequest> requests_;
    mutable std::mutex mutex_;

    void loadFromFile();
    bool flushToFile() const;
    bool appendRequestToFile(const StoredRequest& request) const;
    void printError(const std::string& message) const;
};
