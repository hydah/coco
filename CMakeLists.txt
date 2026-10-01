cmake_minimum_required(VERSION 3.14)

# Must be set before project(); also passed to the OpenSSL build so all objects agree.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(coco VERSION 0.1.0 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(COCO_TOP_LEVEL ON)
else()
  set(COCO_TOP_LEVEL OFF)
endif()

option(COCO_BUILD_EXAMPLES "Build coco examples" ${COCO_TOP_LEVEL})
option(COCO_BUILD_TESTS "Build coco tests" ${COCO_TOP_LEVEL})
# A parent project that embeds coco decides itself what gets installed.
option(COCO_INSTALL "Generate the install rules of the library" ${COCO_TOP_LEVEL})
option(COCO_INSTALL_EXAMPLES "Install the example programs, under bin/examples/" OFF)
option(COCO_USE_SYSTEM_OPENSSL "Link the OpenSSL found by find_package instead of building one" OFF)
option(COCO_ENABLE_ASAN "Build with AddressSanitizer" OFF)

include(GNUInstallDirs)

if(COCO_TOP_LEVEL)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
  endif()
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()

if(COCO_ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# Warnings for coco's own targets; never forced on a parent project.
function(coco_set_warnings target)
  target_compile_options(${target} PRIVATE -Wall)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${target} PRIVATE -fno-ident)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${target} PRIVATE
      -Wno-null-dereference -Wno-sign-conversion -Wno-unused-local-typedef -Wthread-safety)
  endif()
endfunction()

set(THIRDPARTY ${PROJECT_SOURCE_DIR}/thirdparty)

find_package(Threads REQUIRED)
include(cmake/openssl.cmake)
include(cmake/st.cmake)

add_subdirectory(src)

if(COCO_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(COCO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
