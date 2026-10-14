#include "voice_engine/assets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SHA256SUM_PROGRAM
#define SHA256SUM_PROGRAM "/usr/bin/sha256sum"
#endif

extern char** environ;

namespace voice_engine {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t max_manifest_bytes = 4 * 1024 * 1024;

[[noreturn]] void fail(const std::string& context, const std::string& reason) {
    throw ConfigError(context + ": " + reason);
}

std::string string_field(const Json& value, const std::string& name,
                         const std::string& context) {
    if (!value.contains(name) || !value.at(name).is_string()) {
        fail(context + "." + name, "must be a nonempty string");
    }
    auto result = value.at(name).get<std::string>();
    if (result.empty() || result.find('\0') != std::string::npos ||
        std::all_of(result.begin(), result.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        })) {
        fail(context + "." + name, "must be a nonempty string without NUL");
    }
    return result;
}

std::uintmax_t size_field(const Json& value, const std::string& name,
                          const std::string& context) {
    if (!value.contains(name) ||
        !(value.at(name).is_number_unsigned() ||
          (value.at(name).is_number_integer() &&
           value.at(name).get<std::int64_t>() >= 0))) {
        fail(context + "." + name, "must be a nonnegative integer");
    }
    return value.at(name).get<std::uint64_t>();
}

Json load_manifest(const fs::path& path, const std::string& field,
                   const std::string& collection) {
    const auto context = field + " (" + path.string() + ")";
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) {
        fail(context, "manifest is missing or is not a regular file");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(context, "cannot open manifest");
    }
    std::string bytes;
    std::array<char, 16384> buffer {};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
        bytes.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
        if (bytes.size() > max_manifest_bytes) {
            fail(context, "manifest exceeds 4 MiB metadata limit");
        }
    }
    if (!input.eof()) {
        fail(context, "cannot read manifest");
    }
    Json manifest;
    try {
        std::vector<std::set<std::string>> object_keys;
        auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
            if (depth > 64) {
                fail(context, "JSON nesting exceeds 64 levels");
            }
            if (event == Json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == Json::parse_event_t::key) {
                const auto key = parsed.get<std::string>();
                if (!object_keys.back().insert(key).second) {
                    fail(context, "duplicate JSON key: " + key);
                }
            } else if (event == Json::parse_event_t::object_end) {
                object_keys.pop_back();
            }
            return true;
        };
        manifest = Json::parse(bytes, callback);
    } catch (const Json::exception& exception) {
        fail(context, std::string("invalid JSON: ") + exception.what());
    }
    if (!manifest.is_object()) {
        fail(context, "manifest root must be an object");
    }
    if (!manifest.contains("schema_version") ||
        !manifest.at("schema_version").is_number_integer() ||
        manifest.at("schema_version") != 1) {
        fail(context + ".schema_version", "must be integer 1");
    }
    if (!manifest.contains(collection) || !manifest.at(collection).is_array()) {
        fail(context + "." + collection, "must be an array");
    }
    return manifest;
}

std::map<std::string, const Json*> index_manifest(const Json& manifest,
                                                const std::string& collection,
                                                const std::string& context) {
    std::map<std::string, const Json*> entries;
    for (const auto& entry : manifest.at(collection)) {
        if (!entry.is_object()) {
            fail(context + "." + collection, "entries must be objects");
        }
        const auto id = string_field(entry, "id", context + "." + collection);
        if (!entries.emplace(id, &entry).second) {
            fail(context + "." + collection, "duplicate id: " + id);
        }
    }
    return entries;
}

bool within(const fs::path& root, const fs::path& candidate) {
    auto candidate_part = candidate.begin();
    for (const auto& root_part : root) {
        if (candidate_part == candidate.end() || root_part != *candidate_part++) {
            return false;
        }
    }
    return true;
}

class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) : value_(value) {}
    ~FileDescriptor() { close(); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return value_; }
    void close() {
        if (value_ >= 0) {
            ::close(value_);
            value_ = -1;
        }
    }

private:
    int value_;
};

// Keep these descriptors away from stdin/stdout, even when the caller closed one.
int high_descriptor(int descriptor, const std::string& context) {
    if (descriptor < 0) {
        fail(context, std::string("cannot open SHA-256 file descriptor: ") +
             std::strerror(errno));
    }
    if (descriptor >= 3) {
        return descriptor;
    }
    const auto duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    const auto saved_errno = errno;
    ::close(descriptor);
    if (duplicate < 0) {
        fail(context, std::string("cannot duplicate SHA-256 file descriptor: ") +
             std::strerror(saved_errno));
    }
    return duplicate;
}

class SpawnActions {
public:
    explicit SpawnActions(const std::string& context) {
        const int error = posix_spawn_file_actions_init(&value);
        if (error != 0) {
            fail(context, std::string("cannot initialize SHA-256 process: ") +
                 std::strerror(error));
        }
    }
    ~SpawnActions() { posix_spawn_file_actions_destroy(&value); }
    SpawnActions(const SpawnActions&) = delete;
    SpawnActions& operator=(const SpawnActions&) = delete;
    posix_spawn_file_actions_t value {};
};

std::string sha256(const fs::path& path, const std::string& context) {
    const FileDescriptor asset(high_descriptor(open(path.c_str(), O_RDONLY | O_CLOEXEC),
                                               context));
    int pipe_descriptors[2];
    if (pipe2(pipe_descriptors, O_CLOEXEC) != 0) {
        fail(context, std::string("cannot create SHA-256 pipe: ") + std::strerror(errno));
    }
    // Construction of either wrapper must not leak the other end on failure.
    FileDescriptor pipe_input_original(pipe_descriptors[0]);
    FileDescriptor pipe_output_original(pipe_descriptors[1]);
    const int input_copy = fcntl(pipe_input_original.get(), F_DUPFD_CLOEXEC, 3);
    const FileDescriptor output_read(high_descriptor(input_copy, context));
    const int output_copy = fcntl(pipe_output_original.get(), F_DUPFD_CLOEXEC, 3);
    FileDescriptor output_write(high_descriptor(output_copy, context));
    pipe_input_original.close();
    pipe_output_original.close();

    SpawnActions actions(context);
    auto action = [&](int error) {
        if (error != 0) {
            fail(context, std::string("cannot configure SHA-256 process: ") +
                 std::strerror(error));
        }
    };
    action(posix_spawn_file_actions_adddup2(&actions.value, asset.get(), STDIN_FILENO));
    action(posix_spawn_file_actions_adddup2(&actions.value, output_write.get(), STDOUT_FILENO));
    action(posix_spawn_file_actions_addopen(&actions.value, STDERR_FILENO,
                                          "/dev/null", O_WRONLY, 0));
    action(posix_spawn_file_actions_addclose(&actions.value, asset.get()));
    action(posix_spawn_file_actions_addclose(&actions.value, output_read.get()));
    action(posix_spawn_file_actions_addclose(&actions.value, output_write.get()));
    char program[] = SHA256SUM_PROGRAM;
    char binary_option[] = "--binary";
    char* arguments[] = {program, binary_option, nullptr};
    pid_t child = -1;
    const int spawn_error = posix_spawn(&child, program, &actions.value, nullptr,
                                       arguments, environ);
    if (spawn_error != 0) {
        fail(context, std::string("cannot execute SHA-256 checker: ") +
             std::strerror(spawn_error));
    }
    output_write.close();
    // sha256sum streams the asset through its stdin. Neither asset bytes nor a
    // user-provided filename enter argv or a shell; only the short hash is read.
    std::array<char, 256> buffer {};
    std::size_t used = 0;
    bool read_error = false;
    while (used < buffer.size()) {
        const auto amount = read(output_read.get(), buffer.data() + used,
                                 buffer.size() - used);
        if (amount == 0) {
            break;
        }
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            read_error = true;
            break;
        }
        used += static_cast<std::size_t>(amount);
    }
    if (read_error || used == buffer.size()) {
        kill(child, SIGKILL);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || read_error || used == buffer.size() ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(context, "SHA-256 checker failed for file: " + path.string());
    }
    const std::string output(buffer.data(), used);
    if (output.size() != 68 || output.substr(64) != " *-\n" ||
        !std::all_of(output.begin(), output.begin() + 64, [](unsigned char character) {
            return std::isxdigit(character) != 0;
        })) {
        fail(context, "SHA-256 checker returned invalid output");
    }
    return output.substr(0, 64);
}

std::size_t validate_files(const Json& entry, const fs::path& manifest,
                           const std::string& context) {
    if (!entry.contains("files") || !entry.at("files").is_array() ||
        entry.at("files").empty()) {
        fail(context + ".files", "must be a nonempty array");
    }
    std::error_code error;
    const auto root = fs::canonical(manifest.parent_path(), error);
    if (error) {
        fail(context, "cannot resolve manifest directory: " + error.message());
    }
    std::set<fs::path> paths;
    std::size_t count = 0;
    for (const auto& file : entry.at("files")) {
        const auto file_context = context + ".files[" + std::to_string(count) + "]";
        if (!file.is_object()) {
            fail(file_context, "must be an object");
        }
        const fs::path relative(string_field(file, "path", file_context));
        if (relative.is_absolute() || relative.has_root_name() ||
            relative.has_root_directory() ||
            std::any_of(relative.begin(), relative.end(), [](const fs::path& part) {
                return part == "..";
            })) {
            fail(file_context + ".path", "must stay within manifest directory: " +
                 relative.string());
        }
        const auto candidate = fs::canonical(root / relative, error);
        if (error) {
            fail(file_context + ".path", "missing or unresolvable asset file: " +
                 relative.string() + " (" + error.message() + ")");
        }
        if (!within(root, candidate)) {
            fail(file_context + ".path", "asset symlink escapes manifest directory: " +
                 relative.string());
        }
        if (!paths.insert(candidate).second) {
            fail(file_context + ".path", "duplicate file: " + relative.string());
        }
        if (!fs::is_regular_file(candidate, error) || error) {
            fail(file_context + ".path", "asset is not a regular file: " +
                 relative.string());
        }
        const auto expected_size = size_field(file, "size_bytes", file_context);
        const auto actual_size = fs::file_size(candidate, error);
        if (error || actual_size != expected_size) {
            fail(file_context + ".size_bytes", "file size mismatch: " +
                 relative.string());
        }
        auto expected_hash = string_field(file, "sha256", file_context);
        if (expected_hash.size() != 64 ||
            !std::all_of(expected_hash.begin(), expected_hash.end(),
                         [](unsigned char character) {
                             return std::isxdigit(character) != 0;
                         })) {
            fail(file_context + ".sha256", "must contain 64 hexadecimal digits");
        }
        std::transform(expected_hash.begin(), expected_hash.end(), expected_hash.begin(),
                       [](unsigned char character) { return std::tolower(character); });
        if (sha256(candidate, file_context) != expected_hash) {
            fail(file_context + ".sha256", "SHA-256 mismatch: " + relative.string());
        }
        ++count;
    }
    return count;
}

void validate_common_metadata(const Json& entry, const std::string& context) {
    for (const auto* field : {"id", "source_url", "revision", "license"}) {
        string_field(entry, field, context);
    }
}

fs::path resolve_manifest(const Config& config, const std::string& value) {
    const fs::path path(value);
    return path.is_absolute() ? path.lexically_normal()
                             : (config.source_path.parent_path() / path).lexically_normal();
}

}  // namespace

AssetCheckResult validate_assets(const Config& config) {
    AssetCheckResult result;
    const std::array<std::pair<const char*, const ModelRef*>, 5> references {{
        {"vad", &config.models.vad}, {"asr", &config.models.asr},
        {"llm", &config.models.llm}, {"tts", &config.models.tts},
        {"kws", &config.models.kws},
    }};
    if (std::none_of(references.begin(), references.end(), [](const auto& reference) {
            return reference.second->enabled;
        })) {
        return result;
    }

    const auto models_path = resolve_manifest(config, config.assets.models_manifest);
    const auto models_context = "assets.models_manifest (" + models_path.string() + ")";
    const auto models = load_manifest(models_path, "assets.models_manifest", "models");
    const auto model_index = index_manifest(models, "models", models_context);
    std::set<std::string> checked_models;
    std::map<std::string, std::string> required_dependencies;
    for (const auto& reference : references) {
        const auto& model_ref = *reference.second;
        if (!model_ref.enabled) {
            continue;
        }
        const auto found = model_index.find(model_ref.id);
        if (found == model_index.end()) {
            fail("models." + std::string(reference.first) + ".id", "model '" +
                 model_ref.id + "' not found in " + models_path.string());
        }
        if (!checked_models.insert(model_ref.id).second) {
            continue;
        }
        const auto& model = *found->second;
        const auto context = models_context + ".models[" + model_ref.id + "]";
        validate_common_metadata(model, context);
        for (const auto* field : {"platform", "quantization", "toolkit_version",
                                  "runtime_version"}) {
            string_field(model, field, context);
        }
        const auto status = string_field(model, "status", context);
        if (status != "candidate" && status != "verified") {
            fail(context + ".status", "must be candidate or verified");
        }
        if (status == "verified" || model.contains("run_id")) {
            string_field(model, "run_id", context);
        }
        result.files += validate_files(model, models_path, context);
        ++result.models;
        if (model.contains("dependencies")) {
            if (!model.at("dependencies").is_array()) {
                fail(context + ".dependencies", "must be an array");
            }
            std::set<std::string> declared_dependencies;
            for (const auto& dependency : model.at("dependencies")) {
                if (!dependency.is_object()) {
                    fail(context + ".dependencies", "entries must be objects");
                }
                const auto id = string_field(dependency, "id", context + ".dependencies");
                const auto version = string_field(dependency, "version",
                                                  context + ".dependencies[" + id + "]");
                if (!declared_dependencies.insert(id).second) {
                    fail(context + ".dependencies", "duplicate dependency id: " + id);
                }
                const auto inserted = required_dependencies.emplace(id, version);
                if (!inserted.second && inserted.first->second != version) {
                    fail(context + ".dependencies", "conflicting required versions for " + id);
                }
            }
        }
    }
    if (required_dependencies.empty()) {
        return result;
    }
    const auto deps_path = resolve_manifest(config, config.assets.deps_manifest);
    const auto deps_context = "assets.deps_manifest (" + deps_path.string() + ")";
    const auto dependencies = load_manifest(deps_path, "assets.deps_manifest", "dependencies");
    const auto dependency_index = index_manifest(dependencies, "dependencies", deps_context);
    for (const auto& required : required_dependencies) {
        const auto found = dependency_index.find(required.first);
        if (found == dependency_index.end()) {
            fail(deps_context, "missing dependency id: " + required.first);
        }
        const auto& dependency = *found->second;
        const auto context = deps_context + ".dependencies[" + required.first + "]";
        validate_common_metadata(dependency, context);
        if (string_field(dependency, "version", context) != required.second) {
            fail(context + ".version", "required version is " + required.second);
        }
        result.files += validate_files(dependency, deps_path, context);
        ++result.dependencies;
    }
    return result;
}

}  // namespace voice_engine
