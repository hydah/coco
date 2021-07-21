cmake_minimum_required(VERSION 3.1)

project(coco C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

# only build examples if this is the main project
if(CMAKE_PROJECT_NAME STREQUAL "coco")
  option(COCO_BUILD_EXAMPLES "Build coco examples" ON)
endif()

set(CXX_FLAGS "$ENV{CXXFLAGS} -std=c++11 -O0 -Wall -Wno-literal-suffix -fno-ident -D_GNU_SOURCE -D_REENTRNT -g -ggdb")
set(CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__FILENAME__='\"$(subst ${CMAKE_SOURCE_DIR}/,,$(abspath $<))\"'")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
 
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
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

string(TOUPPER ${CMAKE_BUILD_TYPE} BUILD_TYPE)
message(STATUS "CXX_FLAGS = " ${CMAKE_CXX_FLAGS} " " ${CMAKE_CXX_FLAGS_${BUILD_TYPE}})

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

set(THIRDPARTY ${PROJECT_SOURCE_DIR}/thirdparty)
set(ST ${THIRDPARTY}/st)

# openssl
include(cmake/openssl.cmake)
# state thread
include(cmake/st.cmake)

# http parser
exec_program(${CMAKE_COMMAND} ARGS -E copy ${THIRDPARTY}/http-parser/http_parser.h ${PROJECT_SOURCE_DIR}/lib/http-parser/http_parser.h)
include_directories(${PROJECT_SOURCE_DIR}/lib)
include_directories(${PROJECT_SOURCE_DIR}/src)

# coco lib
add_subdirectory(src)

# coco example
if(COCO_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()