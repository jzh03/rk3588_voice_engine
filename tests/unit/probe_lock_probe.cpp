#include "voice_engine/probe_support.hpp"
#include "voice_engine/config.hpp"
#include <iostream>
#include <string>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    int fd = -1;
    try {
        if (argc == 3) {
            if (std::string(argv[2]) != "--self-lock") return 2;
            fd = open(argv[1], O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) return 2;
        }
        voice_engine::probe::require_owned_flock(argv[1]);
        std::cout << "MODEL_LOCK_OK\n";
        if (fd >= 0) close(fd);
        return 0;
    } catch (const voice_engine::ConfigError& error) {
        if (fd >= 0) close(fd);
        std::cerr << error.what() << '\n';
        return 2;
    }
}
