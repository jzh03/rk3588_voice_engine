# Both explicit opt-ins required; default builds never inspect private SDKs.
add_executable(llm_tts_probe tools/llm_tts_probe.cpp)
voice_engine_cpp_target(llm_tts_probe)
target_link_libraries(llm_tts_probe PRIVATE voice_engine_llm voice_engine_tts
    voice_engine_tts_stream voice_engine_probe_support nlohmann_json::nlohmann_json PkgConfig::TTS_SNDFILE)
set_target_properties(llm_tts_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${_llm_sdk}/lib;${_tts_lib}")
if(BUILD_TESTING)
    add_test(NAME llm_tts_probe_cli COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/integration/llm_tts_probe.py" "$<TARGET_FILE:llm_tts_probe>")
    set_tests_properties(llm_tts_probe_cli PROPERTIES TIMEOUT 30)
endif()
