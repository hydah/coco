add_executable(pingpong_client_tcp pingpong_client_tcp.cpp)
target_link_libraries(pingpong_client_tcp coco ssl crypto)
install(TARGETS pingpong_client_tcp RUNTIME DESTINATION ${PROJECT_SOURCE_DIR}/dist/bin/examples/pingpong)

add_executable(pingpong_server_tcp pingpong_server_tcp.cpp)
target_link_libraries(pingpong_server_tcp coco ssl crypto)
install(TARGETS pingpong_server_tcp RUNTIME DESTINATION ${PROJECT_SOURCE_DIR}/dist/bin/examples/pingpong)

add_executable(pingpong_client_udp pingpong_client_udp.cpp)
target_link_libraries(pingpong_client_udp coco ssl crypto)
install(TARGETS pingpong_client_udp RUNTIME DESTINATION ${PROJECT_SOURCE_DIR}/dist/bin/examples/pingpong)

add_executable(pingpong_server_udp pingpong_server_udp.cpp)
target_link_libraries(pingpong_server_udp coco ssl crypto)
install(TARGETS pingpong_server_udp RUNTIME DESTINATION ${PROJECT_SOURCE_DIR}/dist/bin/examples/pingpong)
