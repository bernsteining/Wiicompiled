# PSQ_TEST_EMULATOR (optional) prefixes the command, e.g. node for the web build.
# PSQ_TEST_ABORT_MARKER (optional): expect a failing run whose output contains it, instead of
# exit code 86 (WebAssembly cannot catch abort() with a signal handler).
foreach(operation load store)
    foreach(type 1 2 3)
        foreach(lanes 0 1)
            foreach(route resolved null)
                execute_process(COMMAND ${PSQ_TEST_EMULATOR} "${PSQ_TEST_EXECUTABLE}" ${operation} ${type} ${lanes} ${route}
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
                if(PSQ_TEST_ABORT_MARKER)
                    string(FIND "${output}${error}" "${PSQ_TEST_ABORT_MARKER}" marker)
                    if(NOT result EQUAL 0 AND NOT marker EQUAL -1)
                        continue()
                    endif()
                    message(FATAL_ERROR "Reserved PSQ ${operation}/${type}/${lanes}/${route}: ${result} ${output} ${error}")
                endif()
                if(NOT result EQUAL 86)
                    message(FATAL_ERROR "Reserved PSQ ${operation}/${type}/${lanes}/${route}: ${result} ${output} ${error}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()
