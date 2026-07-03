#pragma once

#include <sqlite3.h>
#include <string>
#include <vector>

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
    explicit Database(const std::string& path = "rlimit.db");
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool isAvailable() const;

    void saveTenant(const std::string& id, const std::string& name,
                    const std::string& algorithm, const std::string& params);
    void deleteTenant(const std::string& id);
    std::vector<TenantRecord> loadAllTenants();

    void logRequest(const std::string& tenantId, long long timestampMs,
                    const std::string& result, const std::string& algorithm,
                    const std::string& stateSnapshot);

    std::vector<RequestRecord> getRequestLog(const std::string& tenantId, int limit = 100);
    int getTotalRequests(const std::string& tenantId);
    int getTotalAllowed(const std::string& tenantId);
    int getTotalDenied(const std::string& tenantId);

private:
    sqlite3* db_;
    bool available_;

    void createTables();
    void printError() const;
    int getCount(const std::string& sql, const std::string& tenantId);
};
