file(REMOVE_RECURSE "${TEST_DIR}")
execute_process(COMMAND "${FIXTURE_WRITER}" --fixture "${TEST_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "study fixture failed")
endif()
execute_process(COMMAND "${ENGINE}" session import "${TEST_DIR}/metadata.json"
    "${TEST_DIR}/capture.json" "${TEST_DIR}/imported" RESULT_VARIABLE result OUTPUT_VARIABLE output)
if(NOT result EQUAL 0 OR NOT output MATCHES "IMPORTED AND REPLAY VERIFIED")
    message(FATAL_ERROR "study import failed: ${output}")
endif()
execute_process(COMMAND "${ENGINE}" session replay "${TEST_DIR}/imported"
    "${TEST_DIR}/imported/replay.json" RESULT_VARIABLE result OUTPUT_VARIABLE output)
if(NOT result EQUAL 0 OR NOT output MATCHES "replay_complete")
    message(FATAL_ERROR "structured replay failed: ${output}")
endif()
execute_process(COMMAND "${ENGINE}" session study "${TEST_DIR}/imported"
    "${TEST_DIR}/imported/replay.json" "${TEST_DIR}/policy.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output)
if(NOT result EQUAL 0 OR NOT output MATCHES "study_complete" OR
   NOT output MATCHES "\"net_settlement_bound_micro_usd\":195100")
    message(FATAL_ERROR "costed CLI study failed: ${output}")
endif()
execute_process(COMMAND "${ENGINE}" session study "${TEST_DIR}/imported"
    "${TEST_DIR}/imported/replay.json" "${TEST_DIR}/missing.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR output MATCHES "study_complete")
    message(FATAL_ERROR "invalid policy falsely completed")
endif()
