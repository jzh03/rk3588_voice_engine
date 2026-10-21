#include "voice_engine/probe_support.hpp"
#include "voice_engine/config.hpp"

#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace voice_engine::probe {
namespace {

std::map<long, bool> flock_ancestors() {
    std::map<long, bool> ancestors;
    long pid = static_cast<long>(getpid());
    for (int depth = 0; pid > 0 && depth < 128; ++depth) {
        if (ancestors.count(pid)) break;
        const auto directory = std::filesystem::path("/proc") / std::to_string(pid);
        std::ifstream input(directory / "stat");
        std::string line;
        if (!std::getline(input, line)) break;
        const auto end = line.rfind(')');
        if (end == std::string::npos) break;
        std::istringstream fields(line.substr(end + 1));
        char state = '\0';
        long parent = 0;
        if (!(fields >> state >> parent)) break;
        std::error_code error;
        const auto executable = std::filesystem::read_symlink(directory / "exe", error);
        ancestors.emplace(pid, !error && executable.filename() == "flock");
        pid = parent;
    }
    return ancestors;
}

[[noreturn]] void missing_lock(const std::filesystem::path& path) {
    throw ConfigError("model validation requires an exclusive lock owned by this "
                      "process or its flock ancestor; run: flock -x " +
                      path.string() + " <probe command>");
}

}  // namespace

void require_owned_flock(const std::filesystem::path& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) missing_lock(path);
    const auto ancestors = flock_ancestors();
    const long current = static_cast<long>(getpid());
    std::ifstream locks("/proc/locks");
    if (!locks) missing_lock(path);
    std::string line;
    while (std::getline(locks, line)) {
        std::istringstream fields(line);
        std::string sequence, kind, advisory, mode, device, start, end;
        long owner = -1;
        if (!(fields >> sequence >> kind >> advisory >> mode >> owner >> device >> start >> end))
            continue;
        if (kind != "FLOCK" || advisory != "ADVISORY" || mode != "WRITE") continue;
        const auto first = device.find(':');
        const auto second = first == std::string::npos ? first : device.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        try {
            const auto dev_major = std::stoull(device.substr(0, first), nullptr, 16);
            const auto dev_minor = std::stoull(device.substr(first + 1, second - first - 1), nullptr, 16);
            const auto inode = std::stoull(device.substr(second + 1), nullptr, 10);
            if (dev_major != static_cast<unsigned long long>(major(info.st_dev)) ||
                dev_minor != static_cast<unsigned long long>(minor(info.st_dev)) ||
                inode != static_cast<unsigned long long>(info.st_ino)) continue;
            const auto found = ancestors.find(owner);
            if (owner == current || (found != ancestors.end() && found->second)) return;
        } catch (const std::exception&) {
            continue;
        }
    }
    missing_lock(path);
}

void require_model_validation_lock() {
    require_owned_flock(kModelValidationLockPath);
}

}  // namespace voice_engine::probe
