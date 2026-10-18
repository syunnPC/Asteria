# SPDX-License-Identifier: Apache-2.0
# Smoke test of one libFuzzer harness in the Fuzz profile.
#
# The replay driver first runs the deterministic regression and writes the
# harness seeds to SEEDS, which campaigns can reuse as a read-only corpus.
# libFuzzer then fuzzes from those seeds for at most RUNS inputs or SECONDS
# seconds; the inputs it keeps and any crash artifact go to WORK, which is
# recreated on every run.
#
# cmake -D REPLAY=<driver> -D FUZZER=<harness> -D SEEDS=<dir> -D WORK=<dir>
#       -D RUNS=<count> -D SECONDS=<limit> -P RunLibFuzzerSmoke.cmake

foreach(variable IN ITEMS REPLAY FUZZER SEEDS WORK RUNS SECONDS)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${SEEDS}" "${WORK}")
file(MAKE_DIRECTORY "${SEEDS}" "${WORK}/Corpus")

execute_process(
    COMMAND "${REPLAY}" --iterations 20000 --write-seeds "${SEEDS}"
    RESULT_VARIABLE replay_result)
if(NOT replay_result EQUAL 0)
    message(FATAL_ERROR "The replay driver failed: ${replay_result}")
endif()

execute_process(
    COMMAND "${FUZZER}"
        -seed=1
        -runs=${RUNS}
        -max_total_time=${SECONDS}
        -timeout=10
        -print_final_stats=1
        "-artifact_prefix=${WORK}/"
        "${WORK}/Corpus"
        "${SEEDS}"
    RESULT_VARIABLE fuzzer_result)
if(NOT fuzzer_result EQUAL 0)
    message(FATAL_ERROR "libFuzzer reported a failure (${fuzzer_result}); see the artifact in ${WORK}")
endif()
