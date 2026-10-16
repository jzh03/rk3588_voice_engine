#include <iostream>
#include <string_view>
#include <pulse/pulseaudio.h>
#include <samplerate.h>
#include <sndfile.h>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

int main() {
    constexpr std::string_view status = "ENV_CHECK_OK";
    auto config = YAML::Load("sample_rate: 16000");
    nlohmann::json result;
    result["sample_rate"] = config["sample_rate"].as<int>();

    std::cout << "libpulse: " << pa_get_library_version() << '\n';
    std::cout << "samplerate: " << src_get_version() << '\n';
    std::cout << "sndfile: " << sf_version_string() << '\n';
    std::cout << result.dump() << '\n';
    std::cout << status << '\n';
}
