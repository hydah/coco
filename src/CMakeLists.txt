# CMake 最低版本号要求
cmake_minimum_required (VERSION 3.5)

# 每层一个静态库，只链接它允许依赖的下层：
#   coco_server -> coco_l7 -> coco_tls -> coco_l4 -> coco_core
# 头文件层面的依赖方向由 tests 里的 LayerDependencies 用例检查（cmake/check_layers.cmake）。

# 协程运行时、错误码、日志、工具。这几个目录互相引用，只能作为一层。
aux_source_directory(./base CORE_SRCS)
aux_source_directory(./common CORE_SRCS)
aux_source_directory(./log CORE_SRCS)
aux_source_directory(./utils CORE_SRCS)
add_library(coco_core ${CORE_SRCS})
target_link_libraries(coco_core PUBLIC st pthread)

# 传输层：套接字封装、TCP、UDP。
aux_source_directory(./net L4_SRCS)
aux_source_directory(./net/layer4 L4_SRCS)
add_library(coco_l4 ${L4_SRCS})
target_link_libraries(coco_l4 PUBLIC coco_core)

# 安全层：TLS，把一个 StreamConn 包成另一个 StreamConn。
aux_source_directory(./net/tls TLS_SRCS)
add_library(coco_tls ${TLS_SRCS})
target_link_libraries(coco_tls PUBLIC coco_l4 ssl crypto)

# 应用层协议：每个协议一个目录，里面是编解码和会话。
aux_source_directory(./net/layer7/http L7_SRCS)
aux_source_directory(./net/layer7/ws L7_SRCS)
add_library(coco_l7 ${L7_SRCS} ${THIRDPARTY}/http-parser/http_parser.c)
target_link_libraries(coco_l7 PUBLIC coco_tls)

# 服务运行框架：accept 循环、连接生命周期、把连接交给协议处理函数。
aux_source_directory(./server SERVER_SRCS)
add_library(coco_server ${SERVER_SRCS})
target_link_libraries(coco_server PUBLIC coco_l7)

set(COCO_LAYER_LIBS coco_core coco_l4 coco_tls coco_l7 coco_server)
foreach(lib ${COCO_LAYER_LIBS})
  add_dependencies(${lib} COCO_OPENSSL_PROJECT)
  set_property(TARGET ${lib} PROPERTY POSITION_INDEPENDENT_CODE ON)
endforeach()

# 全部功能的聚合目标，链接它等于链接所有层。
add_library(coco INTERFACE)
target_link_libraries(coco INTERFACE coco_server)

install(TARGETS ${COCO_LAYER_LIBS} ARCHIVE DESTINATION ${PROJECT_SOURCE_DIR}/dist/lib)
