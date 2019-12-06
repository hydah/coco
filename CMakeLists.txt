cmake_minimum_required(VERSION 2.6)

project(coco C CXX)


if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

# only build examples if this is the main project
if(CMAKE_PROJECT_NAME STREQUAL "coco")
  option(MUDUO_BUILD_EXAMPLES "Build coco examples" ON)
endif()

set(CXX_FLAGS
 -g
 # -DVALGRIND
 -DCHECK_PTHREAD_RETURN_VALUE
 -D_FILE_OFFSET_BITS=64
 -Wall
 -Wextra
 -Werror
 -Wconversion
 -Wno-unused-parameter
#  -Wold-style-cast
 -Woverloaded-virtual
 -Wpointer-arith
 -Wshadow
 -Wwrite-strings
 -march=native
 # -MMD
 -std=c++11
 -rdynamic
#  -fPIC
 )
if(CMAKE_BUILD_BITS EQUAL 32)
  list(APPEND CXX_FLAGS "-m32")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  list(APPEND CXX_FLAGS "-Wno-null-dereference")
  list(APPEND CXX_FLAGS "-Wno-sign-conversion")
  list(APPEND CXX_FLAGS "-Wno-unused-local-typedef")
  list(APPEND CXX_FLAGS "-Wthread-safety")
  list(REMOVE_ITEM CXX_FLAGS "-rdynamic")
endif()
string(REPLACE ";" " " CMAKE_CXX_FLAGS "${CXX_FLAGS}")

set(CMAKE_CXX_FLAGS_DEBUG "-O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)


string(TOUPPER ${CMAKE_BUILD_TYPE} BUILD_TYPE)
message(STATUS "CXX_FLAGS = " ${CMAKE_CXX_FLAGS} " " ${CMAKE_CXX_FLAGS_${BUILD_TYPE}})

# st lib
message("generate st lib")
set(ST_OBJ_DIR ${PROJECT_SOURCE_DIR}/st/obj)
if (${ST_OBJ_DIR}/libst.a)
        message("st-1.9t is ok.")
else()
        message("build st-1.9t")
        execute_process(
            COMMAND bash -c "cd ${PROJECT_SOURCE_DIR}/st && chmod +w * &&
            make linux-debug EXTRA_CFLAGS='-DMD_HAVE_EPOLL'"
        )
endif()
set(LIB_ST  ${ST_OBJ_DIR}/libst.a)
install(FILES ${ST_OBJ_DIR}/st.h DESTINATION include)
install(FILES ${ST_OBJ_DIR}/libst.a DESTINATION lib)

include_directories(${PROJECT_SOURCE_DIR})
add_subdirectory(coco/base)
add_subdirectory(coco/net)

if(MUDUO_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()