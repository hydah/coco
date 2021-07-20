include(ExternalProject)

# 源文件输出临时目录
set(COCO_OPENSSL_ROOT ${CMAKE_SOURCE_DIR}/thirdparty/temp/src_temp/openssl-1.1.1g)

# 编译后头文件和库输出目录
set(BUILD_PREFIX_ROOT ${CMAKE_SOURCE_DIR}/thirdparty/temp/out_libs/openssl-1.1.1g)

# 编译后头文件和库输出目录
set(COCO_OPENSSL_LIB_DIR       ${BUILD_PREFIX_ROOT}/lib)
set(COCO_OPENSSL_INCLUDE_DIR   ${BUILD_PREFIX_ROOT}/include)

set(COCO_OPENSSL_SRC_URL       ${CMAKE_SOURCE_DIR}/thirdparty/openssl-1.1.1g.tar.gz)
set(COCO_OPENSSL_CONFIGURE     cd ${COCO_OPENSSL_ROOT}/src/COCO_OPENSSL_PROJECT/ && ${COCO_OPENSSL_ROOT}/src/COCO_OPENSSL_PROJECT/config no-shared --prefix=${BUILD_PREFIX_ROOT})
set(COCO_OPENSSL_MAKE          cd ${COCO_OPENSSL_ROOT}/src/COCO_OPENSSL_PROJECT/ && make)
set(COCO_OPENSSL_INSTALL       cd ${COCO_OPENSSL_ROOT}/src/COCO_OPENSSL_PROJECT/ && make install)

ExternalProject_Add(COCO_OPENSSL_PROJECT
        URL                   ${COCO_OPENSSL_SRC_URL}
        PREFIX                ${COCO_OPENSSL_ROOT}
        CONFIGURE_COMMAND     ${COCO_OPENSSL_CONFIGURE}
        BUILD_COMMAND         ${COCO_OPENSSL_MAKE}
        INSTALL_COMMAND       ${COCO_OPENSSL_INSTALL}
        )

# 添加头文件和库目录
include_directories(${COCO_OPENSSL_INCLUDE_DIR})
link_directories(${COCO_OPENSSL_LIB_DIR})
