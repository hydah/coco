# One library, libcoco. The layers inside it (core -> codec -> net -> tls | app -> server) are kept
# apart by the include rules that tests/LayerDependencies checks (cmake/check_layers.cmake).
file(GLOB_RECURSE COCO_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/coco/*.cpp)
# macOS archives and some volumes leave "._name.cpp" resource-fork files beside the sources.
list(FILTER COCO_SRCS EXCLUDE REGEX "/\\._[^/]*$")
add_library(coco ${COCO_SRCS} ${THIRDPARTY}/http-parser/http_parser.c)
add_library(coco::coco ALIAS coco)

set_target_properties(coco PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_features(coco PUBLIC cxx_std_11)

# Users include "coco/...". ST, OpenSSL and http-parser stay private: no public header
# includes theirs.
target_include_directories(coco
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${THIRDPARTY})
target_link_libraries(coco PRIVATE st ${COCO_OPENSSL_LIBS} Threads::Threads ${CMAKE_DL_LIBS})

if(APPLE)
  target_compile_definitions(coco PRIVATE _DARWIN_C_SOURCE)
else()
  target_compile_definitions(coco PRIVATE _GNU_SOURCE _REENTRANT)
endif()
coco_set_warnings(coco)

if(NOT COCO_INSTALL)
  return()
endif()

include(CMakePackageConfigHelpers)
set(COCO_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/coco)

install(TARGETS coco EXPORT cocoTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# A static libcoco needs the static libraries it was built with; they go to lib/coco so
# they never shadow a system libssl. A shared one has them linked in.
set(COCO_PC_LIBS_PRIVATE "")
set(COCO_PC_REQUIRES_PRIVATE "")
if(NOT BUILD_SHARED_LIBS)
  install(TARGETS st EXPORT cocoTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/coco)
  string(APPEND COCO_PC_LIBS_PRIVATE " \${libdir}/coco/libst.a")
  if(COCO_USE_SYSTEM_OPENSSL)
    set(COCO_PC_REQUIRES_PRIVATE "openssl")
  else()
    install(FILES ${COCO_OPENSSL_LIB_DIR}/libssl.a ${COCO_OPENSSL_LIB_DIR}/libcrypto.a
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/coco)
    string(APPEND COCO_PC_LIBS_PRIVATE " \${libdir}/coco/libssl.a \${libdir}/coco/libcrypto.a")
  endif()
  string(APPEND COCO_PC_LIBS_PRIVATE " -pthread")
  foreach(lib ${CMAKE_DL_LIBS})
    string(APPEND COCO_PC_LIBS_PRIVATE " -l${lib}")
  endforeach()
endif()

# Headers only used inside the library are not installed.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/coco
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
    PATTERN "*.h"
    PATTERN "*.hpp"
    REGEX "/utils/(utils|md5|sha1|base64)\\.hpp$" EXCLUDE
    REGEX "/base/shutdown\\.hpp$" EXCLUDE
    REGEX "/net/rudp/endpoint\\.hpp$" EXCLUDE)

install(EXPORT cocoTargets NAMESPACE coco:: DESTINATION ${COCO_CMAKE_DIR})

if(BUILD_SHARED_LIBS)
  set(COCO_SHARED ON)
else()
  set(COCO_SHARED OFF)
endif()
configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/cocoConfig.cmake.in
  ${PROJECT_BINARY_DIR}/cocoConfig.cmake
  INSTALL_DESTINATION ${COCO_CMAKE_DIR})
write_basic_package_version_file(${PROJECT_BINARY_DIR}/cocoConfigVersion.cmake
  COMPATIBILITY SameMinorVersion)
install(FILES ${PROJECT_BINARY_DIR}/cocoConfig.cmake ${PROJECT_BINARY_DIR}/cocoConfigVersion.cmake
  DESTINATION ${COCO_CMAKE_DIR})

# Relative to the .pc file, so cmake --install --prefix may pick another prefix.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  set(COCO_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
else()
  file(RELATIVE_PATH COCO_PC_UP "/p/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/p")
  set(COCO_PC_PREFIX "\${pcfiledir}/${COCO_PC_UP}")
endif()
configure_file(${PROJECT_SOURCE_DIR}/cmake/coco.pc.in ${PROJECT_BINARY_DIR}/coco.pc @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/coco.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
