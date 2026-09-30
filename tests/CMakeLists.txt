add_executable(coco_tests
    test_main.cpp
    lifecycle_test.cpp
    coroutine_test.cpp
)
target_include_directories(coco_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(coco_tests coco ssl crypto dl)

# One process per case, so a crash or hang is reported against that case only.
set(COCO_TEST_CASES
    ConnDeletesItselfWhenCycleReturns
    ConnDeletesItselfWhenCycleFails
    ConnDestructorMayYield
    ConnStopDoesNotWait
    ConnStopBeforeFirstRun
    ConnNeverStartedIsOwnedByCaller
    ManagerShutdownWaitsForAllConns
    ManagerDestructorShutsDownConns
    ManagerShutdownSurvivesInterrupt
    ManyConnsExitConcurrently
    ListenStopWaitsForCycle
    ListenStopBeforeStart
    ListenStopFromItself
    ListenStopSurvivesInterrupt
    HttpServerDeletedWhileAccepting
    HttpServerDeletedWithOpenKeepAliveConn
    HttpServerClosesNonKeepAliveConn
    WebSocketClientAfterPeerClose
)
foreach(case ${COCO_TEST_CASES})
    add_test(NAME ${case} COMMAND coco_tests ${case})
    set_tests_properties(${case} PROPERTIES TIMEOUT 10)
endforeach()
