cmake_minimum_required(VERSION 3.16)
project(TaskForge VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

function(taskforge_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(taskforge_audio STATIC src/wav_analyzer.cpp)
target_include_directories(taskforge_audio PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(taskforge_audio PUBLIC cxx_std_17)
target_link_libraries(taskforge_audio PUBLIC Threads::Threads)
taskforge_warnings(taskforge_audio)

add_library(taskforge_core STATIC src/thread_pool.cpp)
target_include_directories(taskforge_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(taskforge_core PUBLIC cxx_std_17)
target_link_libraries(taskforge_core PUBLIC Threads::Threads)
taskforge_warnings(taskforge_core)

add_executable(taskforge app/main.cpp)
target_link_libraries(taskforge PRIVATE taskforge_audio taskforge_core)
taskforge_warnings(taskforge)

add_executable(taskforge_lifecycle_demo examples/lifecycle_demo.cpp)
target_link_libraries(taskforge_lifecycle_demo PRIVATE taskforge_core)
taskforge_warnings(taskforge_lifecycle_demo)

include(CTest)
if(BUILD_TESTING)
    add_executable(taskforge_tests tests/test_wav_analyzer.cpp)
    target_link_libraries(taskforge_tests PRIVATE taskforge_audio)
    taskforge_warnings(taskforge_tests)
    add_test(NAME wav_unit COMMAND taskforge_tests)

    add_executable(taskforge_thread_pool_tests tests/test_thread_pool.cpp)
    target_link_libraries(taskforge_thread_pool_tests PRIVATE taskforge_core)
    taskforge_warnings(taskforge_thread_pool_tests)
    add_test(NAME thread_pool_unit COMMAND taskforge_thread_pool_tests)
    set_tests_properties(thread_pool_unit PROPERTIES TIMEOUT 30)

    add_executable(taskforge_parallel_audio_tests tests/test_parallel_audio.cpp)
    target_link_libraries(taskforge_parallel_audio_tests PRIVATE taskforge_audio taskforge_core)
    taskforge_warnings(taskforge_parallel_audio_tests)
    add_test(NAME parallel_audio_unit COMMAND taskforge_parallel_audio_tests)
    set_tests_properties(parallel_audio_unit PROPERTIES TIMEOUT 30)

    add_executable(taskforge_lifecycle_tests tests/test_task_lifecycle.cpp)
    target_link_libraries(taskforge_lifecycle_tests PRIVATE taskforge_core taskforge_audio)
    taskforge_warnings(taskforge_lifecycle_tests)
    add_test(NAME task_lifecycle_unit
        COMMAND taskforge_lifecycle_tests "${PROJECT_SOURCE_DIR}/samples/tone_440hz_mono_8k.wav")
    set_tests_properties(task_lifecycle_unit PROPERTIES TIMEOUT 30)

    add_test(NAME lifecycle_demo COMMAND taskforge_lifecycle_demo)
    set_tests_properties(lifecycle_demo PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Lifecycle demo: OK")

    add_test(NAME cli_regressions
        COMMAND "${CMAKE_COMMAND}" "-DTASKFORGE_EXE=$<TARGET_FILE:taskforge>"
            "-DSAMPLE_WAV=${PROJECT_SOURCE_DIR}/samples/tone_440hz_mono_8k.wav"
            -P "${PROJECT_SOURCE_DIR}/tests/check_cli.cmake")
    set_tests_properties(cli_regressions PROPERTIES TIMEOUT 30)

    add_test(NAME sample_cli
        COMMAND taskforge "${PROJECT_SOURCE_DIR}/samples/tone_440hz_mono_8k.wav")
    set_tests_properties(sample_cli PROPERTIES PASS_REGULAR_EXPRESSION "rms=")

    add_test(NAME sample_cli_parallel
        COMMAND taskforge --threads 4
            "${PROJECT_SOURCE_DIR}/samples/tone_440hz_mono_8k.wav"
            "${PROJECT_SOURCE_DIR}/samples/tone_440hz_mono_8k.wav")
    set_tests_properties(sample_cli_parallel PROPERTIES PASS_REGULAR_EXPRESSION "mode=parallel")
endif()
