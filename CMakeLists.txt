cmake_minimum_required(VERSION 3.10)
project(SSPOiRS CXX)

set(CMAKE_CXX_STANDARD 14)

add_executable(SSPOiRS main.cpp)

if(WIN32)
    target_link_libraries(SSPOiRS ws2_32)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
else()
    target_link_libraries(SSPOiRS pthread)
endif()