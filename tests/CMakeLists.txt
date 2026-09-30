add_executable(coco_tests
    test_main.cpp
    lifecycle_test.cpp
    coroutine_test.cpp
    tcp_server_test.cpp
    ws_test.cpp
    tls_test.cpp
)
target_include_directories(coco_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(coco_tests PRIVATE COCO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(coco_tests coco dl)

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
    TcpServerEchoes
    TcpServerStopClosesOpenConns
    TcpServerHandlerReturnClosesConn
    TcpServerDeletedWhileAccepting
    TcpServerHandlerSeesShouldStop
    TcpServerServesTls
    TcpServerServeTwiceFails
    WsEncodeLengthForms
    WsEncodeMasksCopyOnly
    WsDecodeFramesSplitAnywhere
    WsDecodeJoinsFragmentsAroundControlFrames
    WsDecodeRejectsBadFrames
    WsDecodeStopsOnHandlerError
    WsClientAnswersPing
    WsClientAnswersClose
    WsClientSendBlockedWhenReadSideEnds
    TlsReadFullyReturnsPlaintext
    TlsConcurrentWriters
)
foreach(case ${COCO_TEST_CASES})
    add_test(NAME ${case} COMMAND coco_tests ${case})
    set_tests_properties(${case} PROPERTIES TIMEOUT 10)
endforeach()

# Headers may only include the same or a lower layer, see cmake/check_layers.cmake.
add_test(NAME LayerDependencies
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${PROJECT_SOURCE_DIR}/src
            -P ${PROJECT_SOURCE_DIR}/cmake/check_layers.cmake)
