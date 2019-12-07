set(
    COCO_BASE

    log.cpp
    utils.cpp
    st_coroutine.cpp
    st_socket.cpp
    error.cpp
  )

add_library(coco_base ${COCO_BASE})
target_link_libraries(coco_base ${LIB_ST})

install(TARGETS coco_base DESTINATION lib)

file(GLOB HEADERS "*.hpp")
install(FILES ${HEADERS} DESTINATION include/coco/base)