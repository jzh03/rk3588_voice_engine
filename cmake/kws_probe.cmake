# KWS reuses the fixed CPU Sherpa/ORT used by the ASR probe's VAD.
add_library(voice_engine_kws STATIC src/kws/sherpa_kws.cpp)
voice_engine_cpp_target(voice_engine_kws)
target_include_directories(voice_engine_kws PRIVATE "${_asr_speech}/include")
target_link_libraries(voice_engine_kws PUBLIC voice_engine_keyword PRIVATE
    voice_engine_probe_support nlohmann_json::nlohmann_json
    "${_asr_sherpa_lib}/libsherpa-onnx-c-api.so")
add_executable(kws_probe tools/kws_probe.cpp)
voice_engine_cpp_target(kws_probe)
target_link_libraries(kws_probe PRIVATE voice_engine_kws voice_engine_assets
    voice_engine_probe_support nlohmann_json::nlohmann_json PkgConfig::ASR_SNDFILE)
set_target_properties(kws_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${_asr_sherpa_lib}")
