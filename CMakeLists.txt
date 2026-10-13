cmake_minimum_required(VERSION 3.16)

project(TaskForge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

add_library(taskforge_audio STATIC
    src/wav_analyzer.cpp
)
target_include_directories(taskforge_audio PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(taskforge_audio PUBLIC cxx_std_17)

add_executable(taskforge app/main.cpp)
target_link_libraries(taskforge PRIVATE taskforge_audio)

if(MSVC)
    target_compile_options(taskforge_audio PRIVATE /W4 /utf-8)
    target_compile_options(taskforge PRIVATE /W4 /utf-8)
else()
    target_compile_options(taskforge_audio PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(taskforge PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(taskforge_tests tests/test_wav_analyzer.cpp)
    target_link_libraries(taskforge_tests PRIVATE taskforge_audio)
    if(MSVC)
        target_compile_options(taskforge_tests PRIVATE /W4 /utf-8)
    else()
        target_compile_options(taskforge_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME wav_unit COMMAND taskforge_tests)
    add_test(
        NAME sample_cli
        COMMAND taskforge "${PROJECT_SOURCE_DIR}/samples/tone_440hz_mono_8k.wav"
    )
    set_tests_properties(sample_cli PROPERTIES PASS_REGULAR_EXPRESSION "rms=")
endif()
