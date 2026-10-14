cmake_minimum_required(VERSION 3.20)
project(rk3588_voice_engine LANGUAGES CXX)

option(VOICE_ENGINE_BUILD_ENGINE "Build the engine and its CLI" ON)
option(VOICE_ENGINE_BUILD_AUDIO "Build the independent mock audio backend" ON)
option(VOICE_ENGINE_BUILD_ENV_CHECK "Build the system dependency check" ON)

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

if(VOICE_ENGINE_BUILD_ENGINE OR VOICE_ENGINE_BUILD_ENV_CHECK)
    find_package(PkgConfig REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

if(VOICE_ENGINE_BUILD_ENV_CHECK)
    # PulseAudio, libsamplerate and libsndfile are confined to this diagnostic.
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
endif()

if(BUILD_TESTING)
    if(VOICE_ENGINE_BUILD_AUDIO)
        add_executable(mock_audio_test tests/unit/mock_audio_test.cpp)
        voice_engine_cpp_target(mock_audio_test)
        target_link_libraries(mock_audio_test PRIVATE voice_engine_audio)
        add_test(NAME mock_audio COMMAND mock_audio_test)
        set_tests_properties(mock_audio PROPERTIES TIMEOUT 30)
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

        add_executable(logger_test tests/unit/logger_test.cpp)
        voice_engine_cpp_target(logger_test)
        target_link_libraries(logger_test PRIVATE voice_engine_logging)
        add_test(NAME jsonl_logger COMMAND logger_test)
        set_tests_properties(jsonl_logger PROPERTIES TIMEOUT 60)

        add_test(NAME build_boundaries
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/build_boundaries.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_COMMAND}"
                    "${CMAKE_CTEST_COMMAND}")
        set_tests_properties(build_boundaries PROPERTIES TIMEOUT 240)
    endif()
endif()
