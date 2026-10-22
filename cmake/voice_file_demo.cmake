# P3 file chain uses the same opt-in, frozen model libraries as coexist_probe.
add_executable(voice_file_demo tools/voice_file_demo.cpp)
voice_engine_cpp_target(voice_file_demo)
target_link_libraries(voice_file_demo PRIVATE voice_engine_asr voice_engine_llm
    voice_engine_tts voice_engine_tts_stream voice_engine_assets voice_engine_probe_support
    nlohmann_json::nlohmann_json PkgConfig::ASR_SNDFILE Threads::Threads)
set_target_properties(voice_file_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${_asr_rknn}/lib;${_llm_sdk}/lib;${_tts_lib}")
if(BUILD_TESTING)
    add_test(NAME voice_file_demo_cli COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/integration/voice_file_demo.py" "$<TARGET_FILE:voice_file_demo>")
    set_tests_properties(voice_file_demo_cli PROPERTIES TIMEOUT 60)
    add_test(NAME voice_file_supervisor COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/integration/voice_file_supervisor.py")
    set_tests_properties(voice_file_supervisor PROPERTIES TIMEOUT 30)
    add_test(NAME voice_file_review COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/unit/test_review_voice_file.py")
    set_tests_properties(voice_file_review PROPERTIES TIMEOUT 30)
endif()
