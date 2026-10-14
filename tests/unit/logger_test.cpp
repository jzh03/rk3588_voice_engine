#include "voice_engine/logger.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using voice_engine::JsonlLogger;
using voice_engine::LogError;
using voice_engine::LogLimits;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Action>
void require_log_error(Action action, const std::string& message) {
    try {
        action();
    } catch (const LogError&) {
        return;
    }
    throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            path = fs::temp_directory_path() /
                   ("voice-engine-logger-test-" + std::to_string(stamp) + "-" +
                    std::to_string(attempt));
            if (fs::create_directory(path)) {
                return;
            }
        }
        throw std::runtime_error("cannot create a temporary test directory");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

std::vector<Json> read_records(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "cannot read a test log file");
    std::vector<Json> records;
    std::string line;
    while (std::getline(input, line)) {
        require(!line.empty(), "JSONL contains an empty line");
        records.push_back(Json::parse(line));
    }
    require(input.eof(), "could not read the complete test log");
    return records;
}

void validate_record(const Json& record) {
    const std::regex timestamp_pattern(
        R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z$)");
    require(record.size() == 11, "unexpected JSONL record fields");
    require(std::regex_match(record.at("timestamp_utc").get<std::string>(),
                             timestamp_pattern),
            "timestamp must contain UTC milliseconds");
    require(record.at("monotonic_ns").is_number_integer(),
            "monotonic timestamp must be an integer");
    require(record.at("monotonic_ns").get<std::int64_t>() > 0,
            "monotonic timestamp must be positive");
    for (const auto* field : {"seq", "session_id", "generation_id"}) {
        require(record.at(field).is_number_unsigned(), "IDs must be unsigned integers");
    }
    for (const auto* field : {"run_id", "level", "event", "state"}) {
        require(record.at(field).is_string(), "JSONL labels must be strings");
    }
    require(record.at("mode") == "mock", "mode must identify mock events");
    require(record.at("data").is_object(), "data must be an object");
}

std::size_t check_quotas(const fs::path& directory, LogLimits limits) {
    std::size_t files = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        require(entry.is_regular_file(), "unexpected entry in the test log directory");
        require(entry.file_size() <= limits.max_file_bytes, "log exceeds byte quota");
        ++files;
    }
    require(files <= limits.max_files, "log exceeds file count quota");
    return files;
}

void test_records_and_escaping(const fs::path& root) {
    const auto run = root / "parents" / "records";
    const std::string escaped = "quote\" slash\\ newline\n tab\t";
    JsonlLogger logger(run, "unit-records");
    const auto before = std::chrono::steady_clock::now().time_since_epoch();
    logger.write("info", "unit_event", 17, 23, "idle", {{"escaped", escaped}});
    logger.write("debug", "unit_default_data", 17, 24, "listening");
    const auto after = std::chrono::steady_clock::now().time_since_epoch();
    const auto records = read_records(run / "events.jsonl");
    require(records.size() == 2, "every write must immediately flush one line");
    for (const auto& record : records) {
        validate_record(record);
        require(record.at("run_id") == "unit-records", "wrong run ID");
        require(record.at("session_id") == 17, "wrong session ID");
        const auto timestamp = record.at("monotonic_ns").get<std::int64_t>();
        require(timestamp >= std::chrono::duration_cast<std::chrono::nanoseconds>(before).count() &&
                    timestamp <= std::chrono::duration_cast<std::chrono::nanoseconds>(after).count(),
                "monotonic timestamp must be measured during write");
    }
    require(records[0].at("seq") == 1 && records[1].at("seq") == 2,
            "sequence must start at one and increase");
    require(records[0].at("data").at("escaped") == escaped,
            "JSON escaping must preserve a synthetic string");
    require(records[0].at("generation_id") == 23 &&
                records[1].at("generation_id") == 24,
            "wrong generation ID");
    require(records[0].at("level") == "info" &&
                records[0].at("event") == "unit_event" &&
                records[0].at("state") == "idle",
            "event labels must be retained");
    require(records[1].at("data").empty(), "default data must be an empty object");
}

void test_rotation(const fs::path& root) {
    const auto run = root / "rotation";
    const LogLimits limits{1200, 3};
    JsonlLogger logger(run, "unit-rotation", limits);
    std::string padding;
    for (unsigned int index = 0; index < 90; ++index) {
        padding += "测";
    }
    for (std::uint64_t sequence = 1; sequence <= 30; ++sequence) {
        logger.write("info", "unit_rotate", 1, 1, "idle",
                     {{"padding", padding}});
        check_quotas(run, limits);
    }
    require(check_quotas(run, limits) == 3, "rotation must retain the configured files");
    std::uint64_t previous = 0;
    std::size_t retained = 0;
    for (const auto* filename : {"events.2.jsonl", "events.1.jsonl", "events.jsonl"}) {
        const auto records = read_records(run / filename);
        require(!records.empty(), "rotated files must contain complete records");
        for (const auto& record : records) {
            validate_record(record);
            const auto sequence = record.at("seq").get<std::uint64_t>();
            require(previous == 0 || sequence == previous + 1,
                    "rotation must preserve chronological sequence");
            previous = sequence;
            ++retained;
        }
    }
    require(previous == 30 && retained < 30, "rotation must retain the newest records");
}

void test_one_file_rotation(const fs::path& root) {
    const auto run = root / "single-file";
    const LogLimits limits{600, 1};
    JsonlLogger logger(run, "unit-single", limits);
    for (unsigned int index = 0; index < 10; ++index) {
        logger.write("info", "unit_rotate", 0, 0, "idle",
                     {{"padding", std::string(150, 'x')}});
        require(check_quotas(run, limits) == 1,
                "a single-file quota must not create backups");
    }
    const auto records = read_records(run / "events.jsonl");
    require(records.size() == 1 && records[0].at("seq") == 10,
            "single-file rotation must replace older contents");
}

void test_directory_ownership(const fs::path& root) {
    const auto run = root / "existing";
    fs::create_directory(run);
    {
        std::ofstream sentinel(run / "preserve.txt");
        sentinel << "preserve";
    }
    require_log_error([&] { JsonlLogger logger(run, "unit-existing"); },
                      "logger must reject an existing run directory");
    require(fs::file_size(run / "preserve.txt") == 8 &&
                !fs::exists(run / "events.jsonl"),
            "rejecting a directory must not modify it");
    const auto empty = root / "existing-empty";
    fs::create_directory(empty);
    require_log_error([&] { JsonlLogger logger(empty, "unit-existing-empty"); },
                      "logger must also reject an empty existing run directory");
    require(fs::is_empty(empty), "an existing empty directory must remain empty");
    const auto file = root / "existing-file";
    { std::ofstream sentinel(file); }
    require_log_error([&] { JsonlLogger logger(file, "unit-existing-file"); },
                      "a regular file cannot become a run directory");
    JsonlLogger logger(root / "trailing-separator" / "", "unit-trailing");
    logger.write("info", "unit_directory", 0, 0, "idle");
    require(read_records(root / "trailing-separator" / "events.jsonl").size() == 1,
            "a trailing directory separator must be accepted for a new run");
}

void test_invalid_limits_and_records(const fs::path& root) {
    require_log_error([&] { JsonlLogger logger(root / "zero-bytes", "unit", {0, 1}); },
                      "zero-byte quota must fail");
    require_log_error([&] { JsonlLogger logger(root / "zero-files", "unit", {1, 0}); },
                      "zero-file quota must fail");
    require(!fs::exists(root / "zero-bytes") && !fs::exists(root / "zero-files"),
            "invalid limits must not create a run directory");
    const auto run = root / "oversized";
    JsonlLogger logger(run, "unit-oversized", {1024, 3});
    require_log_error(
        [&] {
            logger.write("info", "unit_large", 0, 0, "idle",
                         {{"padding", std::string(2048, 'x')}});
        },
        "an oversized JSONL record must be rejected");
    require_log_error(
        [&] { logger.write("info", "unit_array", 0, 0, "idle", Json::array()); },
        "a non-object data payload must be rejected");
    logger.write("info", "unit_after_rejection", 0, 0, "idle");
    const auto records = read_records(run / "events.jsonl");
    require(records.size() == 1 && records[0].at("seq") == 1,
            "rejected records must not write bytes or consume sequence numbers");
    require(check_quotas(run, {1024, 3}) == 1,
            "rejected records must not rotate logs");
}

void test_rotation_io_failure_is_permanent(const fs::path& root) {
    const auto run = root / "rotation-io-failure";
    JsonlLogger logger(run, "unit-rotation-failure", {600, 2});
    const auto blocker = run / "events.1.jsonl";
    fs::create_directory(blocker);
    {
        std::ofstream sentinel(blocker / "block-rotation.txt");
        sentinel << "block";
        require(static_cast<bool>(sentinel), "cannot create the rotation blocker");
    }
    const Json data{{"padding", std::string(150, 'x')}};
    logger.write("info", "unit_before_rotation_failure", 0, 0, "idle", data);
    require_log_error(
        [&] {
            for (unsigned int index = 0; index < 8; ++index) {
                logger.write("info", "unit_trigger_rotation_failure", 0, 0, "idle", data);
            }
        },
        "a nonempty directory blocking rotation must cause a log I/O error");
    const auto bytes_after_failure = fs::file_size(run / "events.jsonl");
    const auto records_after_failure = read_records(run / "events.jsonl");
    require(fs::remove_all(blocker) == 2, "the temporary rotation blocker must be removed");
    try {
        logger.write("info", "unit_after_rotation_failure", 0, 0, "idle");
        throw std::runtime_error("logger must stay failed after the I/O obstacle is removed");
    } catch (const LogError& error) {
        require(std::string(error.what()).find("earlier I/O failure") != std::string::npos,
                "a failed logger must reject writes before retrying file I/O");
    }
    require(fs::file_size(run / "events.jsonl") == bytes_after_failure &&
                read_records(run / "events.jsonl") == records_after_failure &&
                !fs::exists(blocker),
            "a failed logger must not silently resume writing or rotating");
}

void test_concurrent_writes(const fs::path& root) {
    const auto run = root / "concurrent";
    JsonlLogger logger(run, "unit-concurrent");
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (std::uint64_t worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (unsigned int index = 0; index < 25; ++index) {
                    logger.write("debug", "unit_concurrent", worker, 0, "idle");
                }
            } catch (...) {
                failed = true;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(!failed, "concurrent writes must not fail");
    const auto records = read_records(run / "events.jsonl");
    require(records.size() == 100, "concurrent writes must retain every record");
    for (std::size_t index = 0; index < records.size(); ++index) {
        require(records[index].at("seq") == index + 1,
                "concurrent writes must serialize sequence allocation");
    }
}

}  // namespace

int main() {
    try {
        TemporaryDirectory root;
        test_records_and_escaping(root.path);
        test_rotation(root.path);
        test_one_file_rotation(root.path);
        test_directory_ownership(root.path);
        test_invalid_limits_and_records(root.path);
        test_rotation_io_failure_is_permanent(root.path);
        test_concurrent_writes(root.path);
        std::cout << "LOGGER_TEST_OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "logger test failed: " << error.what() << '\n';
        return 1;
    }
}
