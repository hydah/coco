# 检查 src/ 下每一条 #include "..." 都指向同层或更低的层。
# 用法：cmake -DSRC_DIR=<repo>/src -P cmake/check_layers.cmake
cmake_minimum_required(VERSION 3.5...3.28)

if(NOT SRC_DIR)
  message(FATAL_ERROR "usage: cmake -DSRC_DIR=<repo>/src -P check_layers.cmake")
endif()

# 路径正则:层号:层名，按顺序取第一条匹配的。层号大的在上面，只能 include 层号不大于自己的头文件；
# 层号相同但层名不同的是平级层，互相不能 include：tls 和应用层协议的会话（app）都只依赖 net，
# 由协议目录里的 server.* 组合。tls 虽然放在 net/tls/ 里，却比 net 的其余部分高一层，所以
# TcpServer、socket 这些不能用它。不匹配任何一条的路径（st.h、http-parser/、coco/coco_api.h、
# coco/coco.h 等汇总头）不参与检查。
#
# 协议目录（app/ 下的 http、ws、rtmp，以及拨号要用、所以在 net/ 里的 dns）都分三层：codec/ 下
# 是协议本身，不碰连接和协程；目录里的其余文件是会话，在 StreamConn 上驱动 codec；server.*
# 用 TcpServer 组装成服务。
set(COCO_LAYERS
  "^coco/(base|common|log|utils)/:0:core"
  "^coco/(.+/)?codec/:1:codec"
  "^coco/(.+/)?server\\.[^/]+$:4:server"
  "^coco/net/tls/:3:tls"
  "^coco/net/:2:net"
  "^coco/app/:3:app"
)

# codec 不碰协程：除了上面的层号，还不能 include 这些前缀。
set(COCO_CODEC_FORBIDDEN "coco/base/" "coco/coco_api.h" "st.h")
# 汇总头文件包含所有层，库内的文件 include 它就绕过了上面的规则。
set(COCO_FORBIDDEN "coco/coco.h")

# 协议之间默认互不依赖；允许的依赖写成 依赖方:被依赖方。
# WebSocket 靠 HTTP Upgrade 建立，所以 ws 可以用 http。
set(COCO_PROTO_ALLOWED "ws:http")

# Sets LAYER_RANK (-1 when unchecked), LAYER_NAME and PROTO of path. PROTO is the protocol
# directory the file is in, e.g. dns for coco/net/dns/codec/message.hpp, http for
# coco/app/http/server.hpp, or "". net/tls/ is not one: any server.* may use it.
function(coco_layer_of path)
  set(rank -1)
  set(name "")
  foreach(entry ${COCO_LAYERS})
    # if(MATCHES) below overwrites CMAKE_MATCH_<n>, so keep the fields first.
    string(REGEX MATCH "^(.*):([0-9]+):([a-z0-9]+)$" parsed "${entry}")
    set(regex "${CMAKE_MATCH_1}")
    set(entry_rank "${CMAKE_MATCH_2}")
    set(entry_name "${CMAKE_MATCH_3}")
    if(path MATCHES "${regex}")
      set(rank ${entry_rank})
      set(name ${entry_name})
      break()
    endif()
  endforeach()

  set(proto "")
  if(path MATCHES "^coco/(net|app)/([^/]+)/")
    set(proto "${CMAKE_MATCH_2}")
    if(proto STREQUAL "tls")
      set(proto "")
    endif()
  endif()

  set(LAYER_RANK ${rank} PARENT_SCOPE)
  set(LAYER_NAME "${name}" PARENT_SCOPE)
  set(PROTO "${proto}" PARENT_SCOPE)
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
  set(from_proto "${PROTO}")
  math(EXPR checked "${checked} + 1")

  file(STRINGS ${SRC_DIR}/${src} lines REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]")
  foreach(line ${lines})
    string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[\"<]([^\">]+)[\">].*" "\\1" inc "${line}")
    list(FIND COCO_FORBIDDEN "${inc}" forbidden)
    if(NOT forbidden EQUAL -1)
      list(APPEND violations "${src} (${from_name}) -> ${inc} (umbrella header, include what is used)")
    endif()
    if(from_name STREQUAL "codec")
      foreach(prefix ${COCO_CODEC_FORBIDDEN})
        string(LENGTH "${prefix}" prefix_len)
        string(SUBSTRING "${inc}" 0 ${prefix_len} head)
        if(head STREQUAL prefix)
          list(APPEND violations "${src} (codec) -> ${inc} (codec must not use coroutines)")
        endif()
      endforeach()
    endif()

    coco_layer_of("${inc}")
    if(LAYER_RANK EQUAL -1)
      continue()
    endif()
    if(LAYER_RANK GREATER from_rank)
      list(APPEND violations "${src} (${from_name}) -> ${inc} (${LAYER_NAME})")
    elseif(LAYER_RANK EQUAL from_rank AND NOT LAYER_NAME STREQUAL from_name)
      list(APPEND violations "${src} (${from_name}) -> ${inc} (${LAYER_NAME}, same rank)")
    elseif(PROTO AND NOT from_proto STREQUAL PROTO AND (from_proto OR LAYER_NAME STREQUAL "codec"))
      # A protocol's files, its codec included, are only for that protocol and the ones
      # allowed to use it.
      list(FIND COCO_PROTO_ALLOWED "${from_proto}:${PROTO}" allowed)
      if(allowed EQUAL -1)
        list(APPEND violations "${src} (${from_proto}) -> ${inc} (${PROTO})")
      endif()
    endif()
  endforeach()
endforeach()

if(violations)
  string(REPLACE ";" "\n  " report "${violations}")
  message(FATAL_ERROR "layer dependency violations:\n  ${report}")
endif()
message(STATUS "layer dependencies ok, ${checked} files checked")
