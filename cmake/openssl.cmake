# Provides COCO_OPENSSL_LIBS: the system OpenSSL with COCO_USE_SYSTEM_OPENSSL, otherwise
# a static OpenSSL built once from source.
if(COCO_USE_SYSTEM_OPENSSL)
  find_package(OpenSSL 1.1.1 REQUIRED)
  set(COCO_OPENSSL_LIBS OpenSSL::SSL OpenSSL::Crypto)
  return()
endif()

include(ExternalProject)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
include(ProcessorCount)

set(COCO_OPENSSL_VERSION 3.5.9)
set(COCO_OPENSSL_SHA256  603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a)

# --libdir=lib: otherwise 64-bit Linux installs into lib64
set(COCO_OPENSSL_OPTIONS no-shared no-tests no-docs --libdir=lib)
if(APPLE)
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND COCO_OPENSSL_OPTIONS -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
    endif()
else()
    # Always, so the same build also links into a shared libcoco.
    list(APPEND COCO_OPENSSL_OPTIONS -fPIC)
endif()

# Built outside the build directory, so every build directory reuses it; one directory
# per set of options, so differently configured builds do not rebuild each other's.
string(MD5 COCO_OPENSSL_FLAVOR "${COCO_OPENSSL_OPTIONS}")
string(SUBSTRING ${COCO_OPENSSL_FLAVOR} 0 8 COCO_OPENSSL_FLAVOR)
set(COCO_OPENSSL_NAME openssl-${COCO_OPENSSL_VERSION}-${COCO_OPENSSL_FLAVOR})
set(COCO_OPENSSL_ROOT ${PROJECT_SOURCE_DIR}/thirdparty/temp/src_temp/${COCO_OPENSSL_NAME})
set(BUILD_PREFIX_ROOT ${PROJECT_SOURCE_DIR}/thirdparty/temp/out_libs/${COCO_OPENSSL_NAME})
list(APPEND COCO_OPENSSL_OPTIONS --prefix=${BUILD_PREFIX_ROOT})

set(COCO_OPENSSL_LIB_DIR       ${BUILD_PREFIX_ROOT}/lib)
set(COCO_OPENSSL_INCLUDE_DIR   ${BUILD_PREFIX_ROOT}/include)

# 优先使用 thirdparty/ 下的本地源码包（离线构建），否则从 GitHub 下载
set(COCO_OPENSSL_TARBALL ${PROJECT_SOURCE_DIR}/thirdparty/openssl-${COCO_OPENSSL_VERSION}.tar.gz)
if(EXISTS ${COCO_OPENSSL_TARBALL})
    set(COCO_OPENSSL_SRC_URL ${COCO_OPENSSL_TARBALL})
else()
    set(COCO_OPENSSL_SRC_URL
        https://github.com/openssl/openssl/releases/download/openssl-${COCO_OPENSSL_VERSION}/openssl-${COCO_OPENSSL_VERSION}.tar.gz)
endif()

ProcessorCount(COCO_NPROC)
if(COCO_NPROC EQUAL 0)
    set(COCO_NPROC 1)
endif()

ExternalProject_Add(COCO_OPENSSL_PROJECT
        URL                   ${COCO_OPENSSL_SRC_URL}
        URL_HASH              SHA256=${COCO_OPENSSL_SHA256}
        PREFIX                ${COCO_OPENSSL_ROOT}
        BUILD_IN_SOURCE       1
        CONFIGURE_COMMAND     ./config ${COCO_OPENSSL_OPTIONS}
        BUILD_COMMAND         make -j${COCO_NPROC} build_libs
        INSTALL_COMMAND       make install_dev
        BUILD_BYPRODUCTS      ${COCO_OPENSSL_LIB_DIR}/libssl.a ${COCO_OPENSSL_LIB_DIR}/libcrypto.a
        )

# Imported targets must name an existing include directory before the build creates it.
file(MAKE_DIRECTORY ${COCO_OPENSSL_INCLUDE_DIR})

add_library(coco::openssl_crypto STATIC IMPORTED GLOBAL)
set_target_properties(coco::openssl_crypto PROPERTIES
    IMPORTED_LOCATION ${COCO_OPENSSL_LIB_DIR}/libcrypto.a
    INTERFACE_INCLUDE_DIRECTORIES ${COCO_OPENSSL_INCLUDE_DIR}
    INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
add_dependencies(coco::openssl_crypto COCO_OPENSSL_PROJECT)

add_library(coco::openssl_ssl STATIC IMPORTED GLOBAL)
set_target_properties(coco::openssl_ssl PROPERTIES
    IMPORTED_LOCATION ${COCO_OPENSSL_LIB_DIR}/libssl.a
    INTERFACE_INCLUDE_DIRECTORIES ${COCO_OPENSSL_INCLUDE_DIR}
    INTERFACE_LINK_LIBRARIES coco::openssl_crypto)
add_dependencies(coco::openssl_ssl COCO_OPENSSL_PROJECT)

set(COCO_OPENSSL_LIBS coco::openssl_ssl coco::openssl_crypto)
