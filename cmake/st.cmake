add_subdirectory(${ST})
set(ST_INCLUDE_DIR       ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/st)
set(ST_LIB_DIR   ${CMAKE_CURRENT_BINARY_DIR}/lib)

# 添加头文件和库目录
include_directories(${ST_INCLUDE_DIR})
link_directories(${ST_LIB_DIR})