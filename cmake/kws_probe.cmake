add_executable(kws_probe tools/kws_probe.cpp)
voice_engine_cpp_target(kws_probe)
target_link_libraries(kws_probe PRIVATE voice_engine_kws voice_engine_assets
    voice_engine_probe_support nlohmann_json::nlohmann_json PkgConfig::ASR_SNDFILE)
set_target_properties(kws_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${_speech_lib}")
