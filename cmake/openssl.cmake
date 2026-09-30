include(ExternalProject)
include(ProcessorCount)

set(COCO_OPENSSL_VERSION 3.5.9)
set(COCO_OPENSSL_SHA256  603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a)

# 源文件输出临时目录
set(COCO_OPENSSL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/temp/src_temp/openssl-${COCO_OPENSSL_VERSION})

# 编译后头文件和库输出目录
set(BUILD_PREFIX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/temp/out_libs/openssl-${COCO_OPENSSL_VERSION})

set(COCO_OPENSSL_LIB_DIR       ${BUILD_PREFIX_ROOT}/lib)
set(COCO_OPENSSL_INCLUDE_DIR   ${BUILD_PREFIX_ROOT}/include)

# 优先使用 thirdparty/ 下的本地源码包（离线构建），否则从 GitHub 下载
set(COCO_OPENSSL_TARBALL ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openssl-${COCO_OPENSSL_VERSION}.tar.gz)
if(EXISTS ${COCO_OPENSSL_TARBALL})
    set(COCO_OPENSSL_SRC_URL ${COCO_OPENSSL_TARBALL})
else()
    set(COCO_OPENSSL_SRC_URL
        https://github.com/openssl/openssl/releases/download/openssl-${COCO_OPENSSL_VERSION}/openssl-${COCO_OPENSSL_VERSION}.tar.gz)
endif()

# --libdir=lib: otherwise 64-bit Linux installs into lib64
set(COCO_OPENSSL_OPTIONS no-shared no-tests no-docs --prefix=${BUILD_PREFIX_ROOT} --libdir=lib)
if(APPLE)
    list(APPEND COCO_OPENSSL_OPTIONS -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
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

# 添加头文件和库目录
include_directories(${COCO_OPENSSL_INCLUDE_DIR})
link_directories(${COCO_OPENSSL_LIB_DIR})
