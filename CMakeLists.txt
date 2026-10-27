cmake_minimum_required(VERSION 3.20)
project(rk3588_voice_engine LANGUAGES CXX)

option(VOICE_ENGINE_BUILD_ENGINE "Build the engine and its CLI" ON)
option(VOICE_ENGINE_BUILD_AUDIO "Build the independent audio implementations" ON)
option(VOICE_ENGINE_BUILD_PULSE_AUDIO "Build native PulseAudio and streaming DSP" ON)
option(VOICE_ENGINE_BUILD_ENV_CHECK "Build the system dependency check" ON)
option(VOICE_ENGINE_BUILD_ASR_PROBE "Build the optional VAD/RKNN file diagnostic" OFF)
option(VOICE_ENGINE_BUILD_TTS_PROBE "Build the optional CPU TTS file diagnostic" OFF)
option(VOICE_ENGINE_BUILD_LLM_PROBE "Build the optional resident RKLLM diagnostic" OFF)

include(CTest)

function(voice_engine_cpp_target target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
endfunction()

# The interface is usable without either implementation or system audio libraries.
add_library(voice_engine_audio_interface INTERFACE)
target_include_directories(voice_engine_audio_interface INTERFACE include)
target_compile_features(voice_engine_audio_interface INTERFACE cxx_std_17)

if(VOICE_ENGINE_BUILD_AUDIO)
    add_library(voice_engine_audio STATIC src/audio/mock_audio.cpp)
    voice_engine_cpp_target(voice_engine_audio)
    target_link_libraries(voice_engine_audio PUBLIC voice_engine_audio_interface)
endif()

if(VOICE_ENGINE_BUILD_ENGINE OR VOICE_ENGINE_BUILD_ENV_CHECK OR
   (VOICE_ENGINE_BUILD_AUDIO AND VOICE_ENGINE_BUILD_PULSE_AUDIO))
    find_package(PkgConfig REQUIRED)
endif()

if(VOICE_ENGINE_BUILD_ENGINE OR VOICE_ENGINE_BUILD_ENV_CHECK)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

if(VOICE_ENGINE_BUILD_AUDIO AND VOICE_ENGINE_BUILD_PULSE_AUDIO)
    pkg_check_modules(PULSE_AUDIO REQUIRED IMPORTED_TARGET libpulse)
    pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
    find_package(Threads REQUIRED)

    add_library(voice_engine_audio_dsp STATIC src/audio/audio_dsp.cpp)
    voice_engine_cpp_target(voice_engine_audio_dsp)
    target_link_libraries(voice_engine_audio_dsp PUBLIC voice_engine_audio_interface
        PRIVATE PkgConfig::SAMPLERATE)

    add_library(voice_engine_pulse_audio STATIC src/audio/pulse_audio.cpp)
    voice_engine_cpp_target(voice_engine_pulse_audio)
    target_link_libraries(voice_engine_pulse_audio PUBLIC voice_engine_audio_interface
        PRIVATE voice_engine_audio_dsp PkgConfig::PULSE_AUDIO Threads::Threads)
endif()

if(VOICE_ENGINE_BUILD_ENV_CHECK)
    # Dependency checks have no device access; libsndfile is also used by the probe.
    pkg_check_modules(ENV_DEPS REQUIRED IMPORTED_TARGET
        libpulse samplerate sndfile yaml-cpp
    )
    add_executable(env_check tools/env_check.cpp)
    voice_engine_cpp_target(env_check)
    target_link_libraries(env_check PRIVATE PkgConfig::ENV_DEPS
                                           nlohmann_json::nlohmann_json)
    set_target_properties(env_check PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if(VOICE_ENGINE_BUILD_ENGINE)
    pkg_check_modules(YAML_CPP REQUIRED IMPORTED_TARGET yaml-cpp)
    find_package(Threads REQUIRED)
    find_program(SHA256SUM_PROGRAM NAMES sha256sum REQUIRED)

    add_library(voice_engine_config STATIC src/engine/config.cpp)
    voice_engine_cpp_target(voice_engine_config)
    target_include_directories(voice_engine_config PUBLIC include)
    target_link_libraries(voice_engine_config PRIVATE PkgConfig::YAML_CPP)

    add_library(voice_engine_logging STATIC src/engine/logger.cpp)
    voice_engine_cpp_target(voice_engine_logging)
    target_include_directories(voice_engine_logging PUBLIC include)
    target_link_libraries(voice_engine_logging PUBLIC nlohmann_json::nlohmann_json
                                                    Threads::Threads)

    add_library(voice_engine_assets STATIC src/engine/assets.cpp)
    voice_engine_cpp_target(voice_engine_assets)
    target_include_directories(voice_engine_assets PUBLIC include)
    target_link_libraries(voice_engine_assets PUBLIC voice_engine_config
        PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(voice_engine_assets PRIVATE
        SHA256SUM_PROGRAM="${SHA256SUM_PROGRAM}")

    # This is a real build target, independent of the mock audio implementation.
    add_library(voice_engine_engine STATIC src/engine/control.cpp)
    add_library(voice_engine_control ALIAS voice_engine_engine)
    voice_engine_cpp_target(voice_engine_engine)
    target_link_libraries(voice_engine_engine PUBLIC voice_engine_config
        voice_engine_logging voice_engine_assets voice_engine_audio_interface)

    add_library(voice_engine_keyword STATIC src/kws/keyword.cpp)
    voice_engine_cpp_target(voice_engine_keyword)
    target_link_libraries(voice_engine_keyword PUBLIC voice_engine_config)
    add_library(voice_engine_realtime STATIC src/engine/realtime.cpp)
    voice_engine_cpp_target(voice_engine_realtime)
    target_link_libraries(voice_engine_realtime PUBLIC voice_engine_config
        voice_engine_logging voice_engine_audio_interface)

    add_executable(voice_engine src/main.cpp)
    voice_engine_cpp_target(voice_engine)
    target_link_libraries(voice_engine PRIVATE voice_engine_engine)
    if(VOICE_ENGINE_BUILD_AUDIO)
        target_link_libraries(voice_engine PRIVATE voice_engine_audio)
        target_compile_definitions(voice_engine PRIVATE VOICE_ENGINE_HAS_AUDIO=1)
    else()
        target_compile_definitions(voice_engine PRIVATE VOICE_ENGINE_HAS_AUDIO=0)
    endif()
    set_target_properties(voice_engine PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    if(VOICE_ENGINE_BUILD_AUDIO AND VOICE_ENGINE_BUILD_PULSE_AUDIO)
        # File I/O and configuration belong to the diagnostic application;
        # the independently usable native backend has neither dependency.
        pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
        add_executable(audio_probe tools/audio_probe.cpp)
        voice_engine_cpp_target(audio_probe)
        target_link_libraries(audio_probe PRIVATE voice_engine_config
            voice_engine_logging voice_engine_assets voice_engine_pulse_audio voice_engine_audio_dsp
            PkgConfig::SNDFILE)
        set_target_properties(audio_probe PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
endif()

if(BUILD_TESTING)
    if(VOICE_ENGINE_BUILD_AUDIO)
        add_executable(mock_audio_test tests/unit/mock_audio_test.cpp)
        voice_engine_cpp_target(mock_audio_test)
        target_link_libraries(mock_audio_test PRIVATE voice_engine_audio)
        add_test(NAME mock_audio COMMAND mock_audio_test)
        set_tests_properties(mock_audio PROPERTIES TIMEOUT 30)
    endif()

    if(VOICE_ENGINE_BUILD_AUDIO AND VOICE_ENGINE_BUILD_PULSE_AUDIO)
        add_executable(audio_dsp_test tests/unit/audio_dsp_test.cpp)
        voice_engine_cpp_target(audio_dsp_test)
        target_link_libraries(audio_dsp_test PRIVATE voice_engine_audio_dsp)
        add_test(NAME audio_dsp COMMAND audio_dsp_test)
        set_tests_properties(audio_dsp PROPERTIES TIMEOUT 30)
    endif()

    if(VOICE_ENGINE_BUILD_ENGINE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        foreach(script IN ITEMS check_config full_config check_assets control_stdin)
            add_test(NAME ${script}
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/${script}.py"
                        "$<TARGET_FILE:voice_engine>")
            set_tests_properties(${script} PROPERTIES TIMEOUT 60)
        endforeach()

        # Offline, fixed fixture integrity/format regression; no model execution.
        add_test(NAME fixed_fixtures
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/check_fixtures.py")
        set_tests_properties(fixed_fixtures PROPERTIES TIMEOUT 30)

        add_test(NAME fixed_fixtures_negative
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/check_fixtures_negative.py")
        set_tests_properties(fixed_fixtures_negative PROPERTIES TIMEOUT 60)

        add_executable(keyword_test tests/unit/keyword_test.cpp)
        voice_engine_cpp_target(keyword_test)
        target_link_libraries(keyword_test PRIVATE voice_engine_keyword)
        add_test(NAME keyword_config COMMAND keyword_test)

        add_executable(realtime_engine_test tests/unit/realtime_engine_test.cpp)
        voice_engine_cpp_target(realtime_engine_test)
        target_link_libraries(realtime_engine_test PRIVATE voice_engine_realtime)
        add_test(NAME realtime_engine COMMAND realtime_engine_test)
        set_tests_properties(realtime_engine PROPERTIES TIMEOUT 30)

        add_executable(logger_test tests/unit/logger_test.cpp)
        voice_engine_cpp_target(logger_test)
        target_link_libraries(logger_test PRIVATE voice_engine_logging)
        add_test(NAME jsonl_logger COMMAND logger_test)
        set_tests_properties(jsonl_logger PROPERTIES TIMEOUT 60)

        if(VOICE_ENGINE_BUILD_AUDIO AND VOICE_ENGINE_BUILD_PULSE_AUDIO)
            add_test(NAME audio_probe
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/audio_probe.py"
                        "$<TARGET_FILE:audio_probe>")
            set_tests_properties(audio_probe PROPERTIES TIMEOUT 120)
            add_test(NAME audio_lead_in
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/audio_lead_in.py"
                        "$<TARGET_FILE:audio_probe>")
            set_tests_properties(audio_lead_in PROPERTIES TIMEOUT 60)
            add_test(NAME audio_keepalive
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/audio_keepalive.py"
                        "$<TARGET_FILE:audio_probe>")
            set_tests_properties(audio_keepalive PROPERTIES TIMEOUT 90)
        endif()

        add_test(NAME build_boundaries
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/build_boundaries.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_COMMAND}"
                    "${CMAKE_CTEST_COMMAND}")
        set_tests_properties(build_boundaries PROPERTIES TIMEOUT 240)
    endif()
endif()

# File model diagnostics are explicit opt-ins. Default and independent builds
# continue to work without model SDKs, private weights, or model initialization.
if(VOICE_ENGINE_BUILD_ASR_PROBE OR VOICE_ENGINE_BUILD_TTS_PROBE OR VOICE_ENGINE_BUILD_LLM_PROBE)
    if(NOT VOICE_ENGINE_BUILD_ENGINE)
        message(FATAL_ERROR "Model probes require VOICE_ENGINE_BUILD_ENGINE=ON")
    endif()
    add_library(voice_engine_probe_support STATIC src/engine/probe_support.cpp)
    voice_engine_cpp_target(voice_engine_probe_support)
    target_include_directories(voice_engine_probe_support PUBLIC include)

    if(BUILD_TESTING)
        add_executable(probe_lock_probe tests/unit/probe_lock_probe.cpp)
        voice_engine_cpp_target(probe_lock_probe)
        target_link_libraries(probe_lock_probe PRIVATE voice_engine_probe_support)
        add_test(NAME probe_lock
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/probe_lock.py"
                    "$<TARGET_FILE:probe_lock_probe>")
        set_tests_properties(probe_lock PROPERTIES TIMEOUT 30)
    endif()
endif()

if(VOICE_ENGINE_BUILD_ASR_PROBE)
    include(cmake/asr_probe.cmake)
    include(cmake/kws_probe.cmake)
endif()
if(VOICE_ENGINE_BUILD_TTS_PROBE)
    include(cmake/tts_probe.cmake)
endif()
if(VOICE_ENGINE_BUILD_LLM_PROBE)
    include(cmake/llm_probe.cmake)
endif()

if(VOICE_ENGINE_BUILD_LLM_PROBE AND VOICE_ENGINE_BUILD_TTS_PROBE)
    include(cmake/llm_tts_probe.cmake)
endif()

if(VOICE_ENGINE_BUILD_ASR_PROBE AND VOICE_ENGINE_BUILD_LLM_PROBE AND VOICE_ENGINE_BUILD_TTS_PROBE)
    include(cmake/coexist_probe.cmake)
    include(cmake/voice_file_demo.cmake)
endif()


# The real application reuses the exact opt-in model adapters used by P2/P3.
# Default and engine-only builds continue to have no SDK dependency.
if(VOICE_ENGINE_BUILD_ENGINE)
    if(VOICE_ENGINE_BUILD_ASR_PROBE AND VOICE_ENGINE_BUILD_LLM_PROBE AND VOICE_ENGINE_BUILD_TTS_PROBE)
        if(NOT TARGET voice_engine_pulse_audio)
            message(FATAL_ERROR "Realtime engine requires native PulseAudio")
        endif()
        add_library(voice_engine_realtime_models STATIC src/engine/realtime_models.cpp src/engine/realtime_main.cpp)
        voice_engine_cpp_target(voice_engine_realtime_models)
        target_link_libraries(voice_engine_realtime_models PUBLIC voice_engine_realtime
            voice_engine_asr voice_engine_kws voice_engine_llm voice_engine_tts voice_engine_tts_stream
            voice_engine_pulse_audio voice_engine_probe_support voice_engine_assets)
        target_link_libraries(voice_engine PRIVATE voice_engine_realtime_models)
        target_compile_definitions(voice_engine PRIVATE VOICE_ENGINE_HAS_REALTIME=1)
        if(BUILD_TESTING)
            add_test(NAME realtime_preflight COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/integration/realtime_preflight.py" "$<TARGET_FILE:voice_engine>")
            set_tests_properties(realtime_preflight PROPERTIES TIMEOUT 30)
        endif()
        set_target_properties(voice_engine PROPERTIES BUILD_RPATH "${_asr_rknn}/lib;${_llm_sdk}/lib;${_tts_lib}")
    else()
        target_compile_definitions(voice_engine PRIVATE VOICE_ENGINE_HAS_REALTIME=0)
    endif()
endif()
