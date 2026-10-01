# State Threads, built from the submodule as a static library that only coco links.
# Its CMakeLists copies public.h to st.h in its binary directory.
add_subdirectory(${THIRDPARTY}/st ${PROJECT_BINARY_DIR}/thirdparty/st)
target_include_directories(st INTERFACE $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/thirdparty/st>)
