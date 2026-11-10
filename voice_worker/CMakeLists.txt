project(cxxime-voice-worker CXX)

add_executable(cxxime-voice-worker voice_worker.cc)

target_include_directories(cxxime-voice-worker PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/include
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann
)

target_link_directories(cxxime-voice-worker PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/lib
)

target_link_libraries(cxxime-voice-worker PRIVATE sherpa-onnx-c-api)

set_target_properties(cxxime-voice-worker PROPERTIES OUTPUT_NAME zhiyu-voice-worker)

# The worker and its own ONNX Runtime (sherpa-onnx 2.0.1 is built against a newer one than
# the DirectML build zhiyu-server.exe uses) live in voice-runtime/, so the two runtimes never
# meet in one process.
set(VOICE_RUNTIME_DIR ${CMAKE_BINARY_DIR}/voice-runtime)
add_custom_command(TARGET cxxime-voice-worker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VOICE_RUNTIME_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:cxxime-voice-worker> ${VOICE_RUNTIME_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/bin/sherpa-onnx-c-api.dll
            ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/bin/onnxruntime.dll
            ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/bin/onnxruntime_providers_shared.dll
            ${VOICE_RUNTIME_DIR}/
)
