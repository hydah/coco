include(CheckFunctionExists)

set(
    COCO_NET

    server.cpp
    tcp_stack.cpp
  )

add_library(coco_net ${COCO_NET})
target_link_libraries(coco_net coco_base)

install(TARGETS coco_net DESTINATION lib)

set(
    COCO_NET_HEADERS

    server.hpp
    tcp_stack.hpp
    client.hpp
)
install(FILES ${COCO_NET_HEADERS} DESTINATION include/coco/net)
