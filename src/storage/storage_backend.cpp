#include "industrial_mcp/storage/storage_backend.hpp"

#include "industrial_mcp/observability/observability.hpp"

#include <fstream>
#include <mutex>
#include <utility>

#ifdef INDUSTRIAL_MCP_WITH_SQLITE
#include <sqlite3.h>
#endif

namespace industrial_mcp {
namespace {

class JsonlStorageBackend final : public StorageBackend {
public:
    JsonlStorageBackend(std::string alarm_path, std::string audit_path)
        : alarm_path_(std::move(alarm_path)), audit_path_(std::move(audit_path)) {}

    bool append_alarm_json(const Json& alarm) override {
        return append_json_line(alarm_path_, alarm);
    }

    std::vector<Json> load_alarm_json(std::size_t* invalid_count) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Json> records;
        if (invalid_count != nullptr) *invalid_count = 0;
        if (alarm_path_.empty()) return records;
        std::ifstream input(alarm_path_);
        if (!input) return records;
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            try {
                records.push_back(Json::parse(line));
            } catch (...) {
                if (invalid_count != nullptr) ++(*invalid_count);
            }
        }
        return records;
    }

    bool append_audit_json(const Json& audit) override {
        return append_json_line(audit_path_, audit);
    }

    std::string backend_name() const override {
        return "jsonl";
    }

private:
    bool append_json_line(const std::string& path, const Json& value) {
        if (path.empty()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream output(path, std::ios::app);
        if (!output) return false;
        output << value.dump() << '\n';
        return static_cast<bool>(output);
    }

    std::string alarm_path_;
    std::string audit_path_;
    std::mutex mutex_;
};

#ifdef INDUSTRIAL_MCP_WITH_SQLITE
class SqliteStorageBackend final : public StorageBackend {
public:
    SqliteStorageBackend(std::string sqlite_path, std::string alarm_path, std::string audit_path)
        : sqlite_path_(std::move(sqlite_path)),
          fallback_(std::move(alarm_path), std::move(audit_path)) {
        if (sqlite_path_.empty()) return;
        sqlite3* db = nullptr;
        if (sqlite3_open(sqlite_path_.c_str(), &db) != SQLITE_OK) {
            if (db != nullptr) sqlite3_close(db);
            return;
        }
        db_ = db;
        initialize();
    }

    ~SqliteStorageBackend() override {
        if (db_ != nullptr) sqlite3_close(db_);
    }

    bool append_alarm_json(const Json& alarm) override {
        if (db_ == nullptr) return fallback_.append_alarm_json(alarm);
        const bool ok = insert_json("alarm_records", alarm);
        if (alarm.is_object() && alarm.contains("code") && alarm.at("code").is_string() &&
            alarm.at("code").get<std::string>() == "ALARM_ACKNOWLEDGED") {
            insert_json("alarm_acknowledgements", alarm);
        }
        return ok;
    }

    std::vector<Json> load_alarm_json(std::size_t* invalid_count) override {
        if (invalid_count != nullptr) *invalid_count = 0;
        if (db_ == nullptr) return fallback_.load_alarm_json(invalid_count);
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Json> records;
        sqlite3_stmt* statement = nullptr;
        const char* sql = "SELECT payload FROM alarm_records ORDER BY id ASC;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) return records;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
            if (text == nullptr) continue;
            try {
                records.push_back(Json::parse(text));
            } catch (...) {
                if (invalid_count != nullptr) ++(*invalid_count);
            }
        }
        sqlite3_finalize(statement);
        return records;
    }

    bool append_audit_json(const Json& audit) override {
        if (db_ == nullptr) return fallback_.append_audit_json(audit);
        return insert_json("audit_records", audit);
    }

    std::string backend_name() const override {
        return db_ == nullptr ? "jsonl" : "sqlite";
    }

private:
    void initialize() {
        const char* sql =
            "CREATE TABLE IF NOT EXISTS alarm_records (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp TEXT, payload TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS alarm_acknowledgements (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp TEXT, payload TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS audit_records (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp TEXT, payload TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS control_operations (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp TEXT, payload TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS diagnostic_results (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp TEXT, payload TEXT NOT NULL);";
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            if (error != nullptr) sqlite3_free(error);
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    bool insert_json(const char* table, const Json& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (db_ == nullptr) return false;
        const auto sql = std::string("INSERT INTO ") + table + " (timestamp, payload) VALUES (?, ?);";
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) return false;
        const auto timestamp = value.is_object() && value.contains("timestamp") && value.at("timestamp").is_string()
            ? value.at("timestamp").get<std::string>()
            : now_utc_iso8601();
        const auto payload = value.dump();
        sqlite3_bind_text(statement, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, payload.c_str(), -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return ok;
    }

    std::string sqlite_path_;
    sqlite3* db_ = nullptr;
    JsonlStorageBackend fallback_;
    std::mutex mutex_;
};
#endif

bool wants_sqlite(const StorageConfig& storage) {
    return storage.type == "sqlite";
}

} // namespace

std::unique_ptr<StorageBackend> make_storage_backend(const StorageConfig& storage,
                                                     const std::string& alarm_log_path,
                                                     const std::string& audit_log_path) {
#ifdef INDUSTRIAL_MCP_WITH_SQLITE
    if (wants_sqlite(storage)) {
        return std::make_unique<SqliteStorageBackend>(storage.sqlite_path, alarm_log_path, audit_log_path);
    }
#else
    if (wants_sqlite(storage)) {
        emit_structured_log("warn", "sqlite_storage_not_compiled", {{"fallback", "jsonl"}});
    }
#endif
    return std::make_unique<JsonlStorageBackend>(alarm_log_path, audit_log_path);
}

bool sqlite_storage_compiled() {
#ifdef INDUSTRIAL_MCP_WITH_SQLITE
    return true;
#else
    return false;
#endif
}

} // namespace industrial_mcp
