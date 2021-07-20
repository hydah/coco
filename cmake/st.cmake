add_subdirectory(${ST})

exec_program(${CMAKE_COMMAND} ARGS -E copy ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/st/st.h ${PROJECT_SOURCE_DIR}/lib/st/include/st.h)

set(ST_INCLUDE_DIR       ${PROJECT_SOURCE_DIR}/lib/st/include)
set(ST_LIB_DIR   ${PROJECT_SOURCE_DIR}/lib)

# 添加头文件和库目录
include_directories(${ST_INCLUDE_DIR})
link_directories(${ST_LIB_DIR})