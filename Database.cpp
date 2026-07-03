#include "Database.h"
#include <iostream>

Database::Database(const std::string& path)
    : db_(nullptr), available_(false) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB ERROR] " << (db_ ? sqlite3_errmsg(db_) : "cannot open database") << "\n";
        std::cerr << "[DB WARNING] Persistence disabled; Rlimit will continue in memory.\n";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return;
    }

    available_ = true;
    createTables();
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

bool Database::isAvailable() const {
    return available_;
}

void Database::printError() const {
    std::cerr << "[DB ERROR] " << (db_ ? sqlite3_errmsg(db_) : "database unavailable") << "\n";
}

void Database::createTables() {
    if (!available_) return;

    const char* tenantSql =
        "CREATE TABLE IF NOT EXISTS tenants ("
        "id TEXT PRIMARY KEY,"
        "name TEXT NOT NULL,"
        "algorithm TEXT NOT NULL,"
        "params TEXT NOT NULL"
        ");";

    const char* requestSql =
        "CREATE TABLE IF NOT EXISTS request_log ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "tenant_id TEXT NOT NULL,"
        "timestamp_ms INTEGER NOT NULL,"
        "result TEXT NOT NULL,"
        "algorithm TEXT NOT NULL,"
        "state_snapshot TEXT NOT NULL"
        ");";

    char* err = nullptr;
    if (sqlite3_exec(db_, tenantSql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[DB ERROR] " << (err ? err : sqlite3_errmsg(db_)) << "\n";
        sqlite3_free(err);
    }
    err = nullptr;
    if (sqlite3_exec(db_, requestSql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[DB ERROR] " << (err ? err : sqlite3_errmsg(db_)) << "\n";
        sqlite3_free(err);
    }
}

void Database::saveTenant(const std::string& id, const std::string& name,
                          const std::string& algorithm, const std::string& params) {
    if (!available_) return;

    const char* sql =
        "INSERT OR REPLACE INTO tenants (id, name, algorithm, params) "
        "VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        printError();
        return;
    }

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, algorithm.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, params.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) printError();
    sqlite3_finalize(stmt);
}

void Database::deleteTenant(const std::string& id) {
    if (!available_) return;

    const char* sql = "DELETE FROM tenants WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        printError();
        return;
    }

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) printError();
    sqlite3_finalize(stmt);
}

std::vector<TenantRecord> Database::loadAllTenants() {
    std::vector<TenantRecord> tenants;
    if (!available_) return tenants;

    const char* sql = "SELECT id, name, algorithm, params FROM tenants ORDER BY id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        printError();
        return tenants;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TenantRecord rec;
        rec.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        rec.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.algorithm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        rec.params = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        tenants.push_back(rec);
    }

    sqlite3_finalize(stmt);
    return tenants;
}

void Database::logRequest(const std::string& tenantId, long long timestampMs,
                          const std::string& result, const std::string& algorithm,
                          const std::string& stateSnapshot) {
    if (!available_) return;

    const char* sql =
        "INSERT INTO request_log (tenant_id, timestamp_ms, result, algorithm, state_snapshot) "
        "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        printError();
        return;
    }

    sqlite3_bind_text(stmt, 1, tenantId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, timestampMs);
    sqlite3_bind_text(stmt, 3, result.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, algorithm.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, stateSnapshot.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) printError();
    sqlite3_finalize(stmt);
}

std::vector<RequestRecord> Database::getRequestLog(const std::string& tenantId, int limit) {
    std::vector<RequestRecord> records;
    if (!available_) return records;

    const char* sql =
        "SELECT timestamp_ms, result, algorithm, state_snapshot "
        "FROM request_log WHERE tenant_id = ? "
        "ORDER BY id DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        printError();
        return records;
    }

    sqlite3_bind_text(stmt, 1, tenantId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RequestRecord rec;
        rec.timestampMs = sqlite3_column_int64(stmt, 0);
        rec.result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.algorithm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        rec.stateSnapshot = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        records.push_back(rec);
    }

    sqlite3_finalize(stmt);
    return records;
}

int Database::getCount(const std::string& sql, const std::string& tenantId) {
    if (!available_) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        printError();
        return 0;
    }

    sqlite3_bind_text(stmt, 1, tenantId.c_str(), -1, SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    } else {
        printError();
    }

    sqlite3_finalize(stmt);
    return count;
}

int Database::getTotalRequests(const std::string& tenantId) {
    return getCount("SELECT COUNT(*) FROM request_log WHERE tenant_id = ?;", tenantId);
}

int Database::getTotalAllowed(const std::string& tenantId) {
    return getCount("SELECT COUNT(*) FROM request_log WHERE tenant_id = ? AND result = 'ALLOW';", tenantId);
}

int Database::getTotalDenied(const std::string& tenantId) {
    return getCount("SELECT COUNT(*) FROM request_log WHERE tenant_id = ? AND result = 'DENY';", tenantId);
}
