cmake_minimum_required(VERSION 3.16)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB_RECURSE SRC_FILES ${SRC_DIR}/*.c ${SRC_DIR}/*.h)

add_library(tm_lib STATIC ${SRC_FILES})

include (ExternalProject)
ExternalProject_Add(
        tinyserver
        GIT_REPOSITORY https://github.com/vfssoft/tinyserver.git
        GIT_TAG main
        BUILD_IN_SOURCE 1
        CMAKE_ARGS -DUSE_STATIC_OPENSSL=FALSE -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/tinyserver -DBUILD_SHARED_LIBS=OFF
)
add_dependencies(tm_lib tinyserver)

include_directories(../include ${CMAKE_BINARY_DIR}/tinyserver/include)
link_directories(${CMAKE_BINARY_DIR}/tinyserver/lib)

target_link_libraries(tm_lib PRIVATE server_lib)

