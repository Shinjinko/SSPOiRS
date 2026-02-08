cmake_minimum_required(VERSION 3.28)
project(SSPOiRS)

set(CMAKE_CXX_STANDARD 17)

add_executable(SSPOiRS
        main.cpp)

if(WIN32)
    target_link_libraries(SSPOiRS ws2_32)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()