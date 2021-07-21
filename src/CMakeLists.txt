# CMake 最低版本号要求
cmake_minimum_required (VERSION 3.1.0)

aux_source_directory(. SRCS)
aux_source_directory(./base SRCS)
aux_source_directory(./common SRCS)
aux_source_directory(./log SRCS)
aux_source_directory(./net SRCS)
aux_source_directory(./protocol/http SRCS)
aux_source_directory(./net/layer4 SRCS)
aux_source_directory(./utils SRCS)

set(SRCS
    ${SRCS}
    ./net/layer7/coco_http.cpp
)


# 指定生成目标
add_library(coco ${SRCS} ${THIRDPARTY}/http-parser/http_parser.c)
# 添加链接库
target_link_libraries(coco libst.a pthread)
set_property(TARGET coco PROPERTY POSITION_INDEPENDENT_CODE ON)
install(TARGETS coco ARCHIVE DESTINATION ${PROJECT_SOURCE_DIR}/dist/lib)
