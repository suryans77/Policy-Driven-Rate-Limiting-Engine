#include "Database.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace {
std::string encodeField(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return out.str();
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decodeField(const std::string& value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int high = hexValue(value[i + 1]);
            int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> parts;
    std::stringstream ss(line);
    std::string part;
    while (std::getline(ss, part, '\t')) {
        parts.push_back(part);
    }
    return parts;
}
}

Database::Database(const std::string& path)
    : path_(path),
      available_(false)
{
    std::ofstream create(path_, std::ios::app);
    if (!create) {
        printError("unable to open " + path_);
        std::cerr << "[DB WARNING] File persistence disabled; Rlimit will continue in memory.\n";
        return;
    }
    available_ = true;
    create.close();
    loadFromFile();
}

bool Database::isAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_;
}

void Database::printError(const std::string& message) const {
    std::cerr << "[DB ERROR] " << message << "\n";
}

void Database::loadFromFile() {
    tenants_.clear();
    requests_.clear();

    std::ifstream in(path_);
    if (!in) {
        available_ = false;
        printError("unable to read " + path_);
        return;
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        auto parts = splitTabs(line);
        if (parts.empty()) continue;

        if (parts[0] == "TENANT" && parts.size() == 5) {
            TenantRecord rec;
            rec.id = decodeField(parts[1]);
            rec.name = decodeField(parts[2]);
            rec.algorithm = decodeField(parts[3]);
            rec.params = decodeField(parts[4]);
            tenants_.push_back(rec);
        } else if (parts[0] == "REQUEST" && parts.size() == 6) {
            try {
                std::size_t consumed = 0;
                long long timestamp = std::stoll(parts[2], &consumed);
                if (consumed != parts[2].size()) throw std::invalid_argument("trailing timestamp data");
                StoredRequest stored;
                stored.tenantId = decodeField(parts[1]);
                stored.record.timestampMs = timestamp;
                stored.record.result = decodeField(parts[3]);
                stored.record.algorithm = decodeField(parts[4]);
                stored.record.stateSnapshot = decodeField(parts[5]);
                requests_.push_back(stored);
            } catch (const std::exception&) {
                printError("ignoring malformed request record at line "
                           + std::to_string(lineNumber));
            }
        }
    }
}

bool Database::flushToFile() const {
    if (!available_) return false;

    const std::string temporaryPath = path_ + ".tmp";
    std::ofstream out(temporaryPath, std::ios::trunc);
    if (!out) {
        printError("unable to write " + temporaryPath);
        return false;
    }

    for (const auto& tenant : tenants_) {
        out << "TENANT"
            << '\t' << encodeField(tenant.id)
            << '\t' << encodeField(tenant.name)
            << '\t' << encodeField(tenant.algorithm)
            << '\t' << encodeField(tenant.params)
            << '\n';
    }

    for (const auto& request : requests_) {
        out << "REQUEST"
            << '\t' << encodeField(request.tenantId)
            << '\t' << request.record.timestampMs
            << '\t' << encodeField(request.record.result)
            << '\t' << encodeField(request.record.algorithm)
            << '\t' << encodeField(request.record.stateSnapshot)
            << '\n';
    }

    out.close();
    if (!out) {
        printError("unable to finish writing " + temporaryPath);
        std::remove(temporaryPath.c_str());
        return false;
    }

#ifdef _WIN32
    bool replaced = false;
    for (int attempt = 0; attempt < 5 && !replaced; ++attempt) {
        replaced = MoveFileExA(temporaryPath.c_str(), path_.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!replaced) Sleep(static_cast<DWORD>(10 * (attempt + 1)));
    }
    if (!replaced) {
        printError("unable to atomically replace " + path_);
        std::remove(temporaryPath.c_str());
        return false;
    }
#else
    if (std::rename(temporaryPath.c_str(), path_.c_str()) != 0) {
        printError("unable to atomically replace " + path_);
        std::remove(temporaryPath.c_str());
        return false;
    }
#endif
    return true;
}

bool Database::appendRequestToFile(const StoredRequest& request) const {
    std::ofstream out(path_, std::ios::app);
    if (!out) return false;
    out << "REQUEST"
        << '\t' << encodeField(request.tenantId)
        << '\t' << request.record.timestampMs
        << '\t' << encodeField(request.record.result)
        << '\t' << encodeField(request.record.algorithm)
        << '\t' << encodeField(request.record.stateSnapshot)
        << '\n';
    return static_cast<bool>(out);
}

bool Database::saveTenant(const std::string& id, const std::string& name,
                          const std::string& algorithm, const std::string& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) return false;

    auto it = std::find_if(tenants_.begin(), tenants_.end(),
        [&](const TenantRecord& rec) { return rec.id == id; });

    TenantRecord rec{id, name, algorithm, params};
    if (it == tenants_.end()) {
        tenants_.push_back(rec);
    } else {
        *it = rec;
    }
    if (!flushToFile()) {
        available_ = false;
        return false;
    }
    return true;
}

bool Database::deleteTenant(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) return false;

    tenants_.erase(std::remove_if(tenants_.begin(), tenants_.end(),
        [&](const TenantRecord& rec) { return rec.id == id; }), tenants_.end());
    if (!flushToFile()) {
        available_ = false;
        return false;
    }
    return true;
}

std::vector<TenantRecord> Database::loadAllTenants() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) return {};

    auto tenants = tenants_;
    std::sort(tenants.begin(), tenants.end(),
        [](const TenantRecord& a, const TenantRecord& b) {
            return a.id < b.id;
        });
    return tenants;
}

bool Database::logRequest(const std::string& tenantId, long long timestampMs,
                          const std::string& result, const std::string& algorithm,
                          const std::string& stateSnapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) return false;

    StoredRequest stored;
    stored.tenantId = tenantId;
    stored.record.timestampMs = timestampMs;
    stored.record.result = result;
    stored.record.algorithm = algorithm;
    stored.record.stateSnapshot = stateSnapshot;
    requests_.push_back(stored);
    constexpr std::size_t maxAuditRecords = 100000;
    if (requests_.size() > maxAuditRecords) {
        requests_.erase(requests_.begin(),
                        requests_.begin() + static_cast<std::ptrdiff_t>(
                            requests_.size() - maxAuditRecords));
        if (!flushToFile()) {
            printError("unable to compact audit store");
            available_ = false;
            return false;
        }
    } else if (!appendRequestToFile(stored)) {
        printError("unable to append request to " + path_);
        available_ = false;
        return false;
    }
    return true;
}

std::vector<RequestRecord> Database::getRequestLog(const std::string& tenantId, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RequestRecord> records;
    if (!available_) return records;

    for (auto it = requests_.rbegin(); it != requests_.rend(); ++it) {
        if (it->tenantId != tenantId) continue;
        records.push_back(it->record);
        if (static_cast<int>(records.size()) >= limit) break;
    }
    return records;
}

int Database::getTotalRequests(const std::string& tenantId) {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& request : requests_) {
        if (request.tenantId == tenantId) ++count;
    }
    return count;
}

int Database::getTotalAllowed(const std::string& tenantId) {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& request : requests_) {
        if (request.tenantId == tenantId && request.record.result == "ALLOW") ++count;
    }
    return count;
}

int Database::getTotalDenied(const std::string& tenantId) {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& request : requests_) {
        if (request.tenantId == tenantId && request.record.result == "DENY") ++count;
    }
    return count;
}
