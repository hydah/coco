# 检查 src/ 下每一条 #include "..." 都指向同层或更低的层。
# 用法：cmake -DSRC_DIR=<repo>/src -P cmake/check_layers.cmake
cmake_minimum_required(VERSION 3.5...3.28)

if(NOT SRC_DIR)
  message(FATAL_ERROR "usage: cmake -DSRC_DIR=<repo>/src -P check_layers.cmake")
endif()

# 路径前缀:层号:层名。层号大的在上面，只能 include 层号不大于自己的头文件。
# 不在表里的路径（st.h、http-parser/、coco_api.h）不参与检查。
set(COCO_LAYERS
  "base/:0:core"
  "common/:0:core"
  "log/:0:core"
  "utils/:0:core"
  "net/coco_socket:1:l4"
  "net/layer4/:1:l4"
  "net/tls/:2:tls"
  "net/layer7/:2:l7"
  "server/:3:server"
)
# 层号相同但层名不同的是平级层，互相不能 include：tls 和 l7 都只依赖 l4，由 server 组合。

# layer7 下每个目录是一个协议，协议之间默认互不依赖；允许的依赖写成 依赖方:被依赖方。
# WebSocket 靠 HTTP Upgrade 建立，所以 ws 可以用 http。
set(COCO_L7_ALLOWED "ws:http")

# Sets LAYER_RANK (-1 when unchecked), LAYER_NAME and L7_PROTO for path.
function(coco_layer_of path)
  set(rank -1)
  set(name "")
  foreach(entry ${COCO_LAYERS})
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 prefix)
    string(LENGTH "${prefix}" prefix_len)
    string(SUBSTRING "${path}" 0 ${prefix_len} head)
    if(head STREQUAL prefix)
      list(GET parts 1 rank)
      list(GET parts 2 name)
      break()
    endif()
  endforeach()

  set(proto "")
  if(path MATCHES "^net/layer7/([^/]+)/")
    set(proto "${CMAKE_MATCH_1}")
  endif()

  set(LAYER_RANK ${rank} PARENT_SCOPE)
  set(LAYER_NAME "${name}" PARENT_SCOPE)
  set(L7_PROTO "${proto}" PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE sources RELATIVE ${SRC_DIR}
  ${SRC_DIR}/*.h ${SRC_DIR}/*.hpp ${SRC_DIR}/*.c ${SRC_DIR}/*.cpp)

set(violations "")
set(checked 0)
foreach(src ${sources})
  coco_layer_of("${src}")
  if(LAYER_RANK EQUAL -1)
    continue()
  endif()
  set(from_rank ${LAYER_RANK})
  set(from_name ${LAYER_NAME})
  set(from_proto "${L7_PROTO}")
  math(EXPR checked "${checked} + 1")

  file(STRINGS ${SRC_DIR}/${src} lines REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
  foreach(line ${lines})
    string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\".*" "\\1" inc "${line}")
    coco_layer_of("${inc}")
    if(LAYER_RANK EQUAL -1)
      continue()
    endif()

    if(LAYER_RANK GREATER from_rank)
      list(APPEND violations "${src} (${from_name}) -> ${inc} (${LAYER_NAME})")
    elseif(LAYER_RANK EQUAL from_rank AND NOT LAYER_NAME STREQUAL from_name)
      list(APPEND violations "${src} (${from_name}) -> ${inc} (${LAYER_NAME}, same rank)")
    elseif(from_proto AND L7_PROTO AND NOT from_proto STREQUAL L7_PROTO)
      list(FIND COCO_L7_ALLOWED "${from_proto}:${L7_PROTO}" allowed)
      if(allowed EQUAL -1)
        list(APPEND violations "${src} (l7/${from_proto}) -> ${inc} (l7/${L7_PROTO})")
      endif()
    endif()
  endforeach()
endforeach()

if(violations)
  string(REPLACE ";" "\n  " report "${violations}")
  message(FATAL_ERROR "layer dependency violations:\n  ${report}")
endif()
message(STATUS "layer dependencies ok, ${checked} files checked")
