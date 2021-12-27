# add_executable(http_server http_server.cpp)
# target_link_libraries(http_server coco ssl crypto dl)
# install(TARGETS http_server RUNTIME DESTINATION ${PROJECT_SOURCE_DIR}/dist/bin/examples/http/)
# install(FILES ${PROJECT_SOURCE_DIR}/examples/http-server/server.key
#     DESTINATION ${PROJECT_SOURCE_DIR}/dist/bin/examples/http/
#     )
# install(FILES ${PROJECT_SOURCE_DIR}/examples/http-server/server.crt
#     DESTINATION ${PROJECT_SOURCE_DIR}/dist/bin/examples/http/
#     )

add_executable(ws_client ws_client.cpp)
target_link_libraries(ws_client coco ssl crypto dl)
install(TARGETS ws_client RUNTIME DESTINATION ${PROJECT_SOURCE_DIR}/dist/bin/examples/websocket/)
