if(NOT DEFINED TASKFORGE_EXE OR NOT DEFINED SAMPLE_WAV)
    message(FATAL_ERROR "TASKFORGE_EXE and SAMPLE_WAV are required")
endif()

function(check_case expected_code expected_pattern)
    execute_process(
        COMMAND "${TASKFORGE_EXE}" ${ARGN}
        RESULT_VARIABLE actual_code
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 10
    )
    if(NOT "${actual_code}" STREQUAL "${expected_code}")
        message(FATAL_ERROR "Exit code ${actual_code}, expected ${expected_code}: ${ARGN}\n${output}${errors}")
    endif()
    if(NOT "${output}${errors}" MATCHES "${expected_pattern}")
        message(FATAL_ERROR "Missing expected output ${expected_pattern}: ${ARGN}\n${output}${errors}")
    endif()
endfunction()

check_case(0 "TaskForge v0\\.3\\.0" --version)
check_case(0 "Per-result wait budget" --help)
check_case(2 "Provide at least one WAV file")
check_case(2 "Worker count must be an integer" --threads 0 "${SAMPLE_WAV}")
check_case(2 "Worker count must be an integer" --threads abc "${SAMPLE_WAV}")
check_case(2 "Worker count must be an integer" --threads 65 "${SAMPLE_WAV}")
check_case(2 "requires one worker count" --threads)
check_case(2 "Specify an execution mode only once" --threads 2 --serial "${SAMPLE_WAV}")
check_case(2 "Wait budget must be an integer" --threads 1 --wait-ms 0 "${SAMPLE_WAV}")
check_case(2 "Wait budget must be an integer" --threads 1 --wait-ms abc "${SAMPLE_WAV}")
check_case(2 "Wait budget must be an integer" --threads 1 --wait-ms 600001 "${SAMPLE_WAV}")
check_case(2 "requires --threads N" --wait-ms 10 "${SAMPLE_WAV}")
check_case(2 "requires one wait budget" --threads 1 --wait-ms)
check_case(2 "requires one wait budget" --threads 1 --wait-ms 10 --wait-ms 20 "${SAMPLE_WAV}")
check_case(2 "Unknown option" --unknown "${SAMPLE_WAV}")
check_case(0 "rms=0\\.353551" --threads 1 --wait-ms 600000 "${SAMPLE_WAV}")

execute_process(COMMAND "${TASKFORGE_EXE}" --serial "${SAMPLE_WAV}" "${SAMPLE_WAV}"
    RESULT_VARIABLE serial_code OUTPUT_VARIABLE serial_output ERROR_VARIABLE serial_errors TIMEOUT 10)
execute_process(COMMAND "${TASKFORGE_EXE}" --threads 4 "${SAMPLE_WAV}" "${SAMPLE_WAV}"
    RESULT_VARIABLE parallel_code OUTPUT_VARIABLE parallel_output ERROR_VARIABLE parallel_errors TIMEOUT 10)
string(REGEX REPLACE "^[^\n]*\n" "" serial_body "${serial_output}")
string(REGEX REPLACE "^[^\n]*\n" "" parallel_body "${parallel_output}")
if(NOT serial_code EQUAL 0 OR NOT parallel_code EQUAL 0 OR
   NOT "${serial_body}" STREQUAL "${parallel_body}" OR
   NOT "${serial_errors}${parallel_errors}" STREQUAL "")
    message(FATAL_ERROR "Serial/parallel CLI output mismatch")
endif()

set(missing "${SAMPLE_WAV}.intentionally_missing")
if(EXISTS "${missing}")
    message(FATAL_ERROR "Test requires a nonexistent path: ${missing}")
endif()
execute_process(COMMAND "${TASKFORGE_EXE}" --threads 2 "${SAMPLE_WAV}" "${missing}" "${SAMPLE_WAV}"
    RESULT_VARIABLE mixed_code OUTPUT_VARIABLE mixed_output ERROR_VARIABLE mixed_errors TIMEOUT 10)
string(REGEX MATCHALL "rms=0\\.353551" successes "${mixed_output}")
list(LENGTH successes success_count)
if(NOT mixed_code EQUAL 1 OR NOT success_count EQUAL 2 OR NOT mixed_errors MATCHES "Cannot open input file")
    message(FATAL_ERROR "Mixed batch must preserve both valid results and exit with 1")
endif()

message(STATUS "CLI version, parameter rejection, serial/parallel equivalence and mixed errors: OK")
