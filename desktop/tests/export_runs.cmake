# Solves MODEL twice in the real KAIRO Desktop application (automatic
# dispatch, then forced barrier) and exports each run record to OUT.
file(MAKE_DIRECTORY ${OUT})
foreach(run auto barrier)
    set(engine_args)
    if(run STREQUAL "barrier")
        set(engine_args --engine barrier)
    endif()
    execute_process(
        COMMAND ${KAIRO} -platform offscreen --capture ${MODEL} ${engine_args}
                --out ${OUT}/${run}.png --export ${OUT}/${run}.json --runs-dir ${OUT}/runs
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "KAIRO --capture (${run}) failed: ${result}")
    endif()
endforeach()
