# CPU VAD and KWS are shared by both realtime modes.
set(_speech_base "${PROJECT_SOURCE_DIR}/deps/model-prep/speech")
set(_speech_lib "${_speech_base}/sherpa-onnx-v1.12.14-linux-aarch64-shared-cpu/lib")
set(_speech_ort "${_speech_base}/onnxruntime/onnxruntime-linux-aarch64-glibc2_17-Release-1.17.1-patched/include")
foreach(_file IN ITEMS "${_speech_base}/include/sherpa-onnx/c-api/c-api.h"
        "${_speech_ort}/onnxruntime_c_api.h" "${_speech_lib}/libsherpa-onnx-c-api.so"
        "${_speech_lib}/libonnxruntime.so")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "CPU VAD/KWS requires fixed private dependency: ${_file}")
    endif()
endforeach()

add_library(voice_engine_listening_asr STATIC src/asr/listening_asr.cpp)
voice_engine_cpp_target(voice_engine_listening_asr)
target_include_directories(voice_engine_listening_asr PUBLIC include)
add_library(voice_engine_vad STATIC src/asr/silero_vad.cpp)
voice_engine_cpp_target(voice_engine_vad)
target_include_directories(voice_engine_vad PUBLIC include PRIVATE "${_speech_base}/include" "${_speech_ort}")
target_link_libraries(voice_engine_vad PUBLIC voice_engine_config
    PRIVATE voice_engine_probe_support nlohmann_json::nlohmann_json
    "${_speech_lib}/libsherpa-onnx-c-api.so" "${_speech_lib}/libonnxruntime.so")

add_library(voice_engine_kws STATIC src/kws/sherpa_kws.cpp)
voice_engine_cpp_target(voice_engine_kws)
target_include_directories(voice_engine_kws PRIVATE "${_speech_base}/include")
target_link_libraries(voice_engine_kws PUBLIC voice_engine_keyword PRIVATE
    voice_engine_probe_support nlohmann_json::nlohmann_json
    "${_speech_lib}/libsherpa-onnx-c-api.so")
