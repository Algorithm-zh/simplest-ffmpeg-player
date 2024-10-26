cmake_minimum_required(VERSION 3.0)
project(player)
file(GLOB SRC_LIST ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
set(CMAKE_CXX_STANDARD 17)
include_directories(${PROJECT_SOURCE_DIR}/include
                    /usr/include/SDL2
                    /usr/local/include/libavformat
                    /usr/local/include/libavcodec
                    /usr/local/include/libswscale
                    /usr/local/include/libavutil
                    )
link_directories(/usr/local/lib)
add_executable(player ${SRC_LIST})
target_link_libraries(player avutil avformat avcodec swscale swresample SDL2 pthread)
