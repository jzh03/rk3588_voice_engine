# Enabled only when all three fixed model probe families are explicitly enabled.
add_executable(coexist_probe tools/coexist_probe.cpp)
voice_engine_cpp_target(coexist_probe)
target_link_libraries(coexist_probe PRIVATE voice_engine_asr voice_engine_llm
    voice_engine_tts voice_engine_tts_stream voice_engine_assets voice_engine_probe_support
    nlohmann_json::nlohmann_json PkgConfig::ASR_SNDFILE Threads::Threads)
set_target_properties(coexist_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${_asr_rknn}/lib;${_llm_sdk}/lib;${_tts_lib}")
if(BUILD_TESTING)
    add_test(NAME coexist_probe_cli COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/integration/coexist_probe.py" "$<TARGET_FILE:coexist_probe>")
    set_tests_properties(coexist_probe_cli PROPERTIES TIMEOUT 30)
    add_test(NAME coexist_supervisor COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/integration/coexist_supervisor.py")
    set_tests_properties(coexist_supervisor PROPERTIES TIMEOUT 30)
endif()
