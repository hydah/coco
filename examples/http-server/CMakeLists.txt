coco_example(http_server http_server.cpp)
coco_example(http_client http_client.cpp)

# http_server reads ./server.key and ./server.crt, so they go next to it.
if(COCO_INSTALL_EXAMPLES)
  install(FILES server.key server.crt DESTINATION ${CMAKE_INSTALL_BINDIR}/examples/http-server)
endif()
