add_executable(http_server http_server.cpp)
target_link_libraries(http_server coco)

add_executable(http_client http_client.cpp)
target_link_libraries(http_client coco)