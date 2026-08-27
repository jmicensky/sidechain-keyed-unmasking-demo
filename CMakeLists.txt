cmake_minimum_required(VERSION 3.10)
project(demo_engine_cli CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

add_executable(demo_engine_cli src/main.cpp)
target_include_directories(demo_engine_cli PRIVATE include third_party)

find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(demo_engine_cli PRIVATE ${MATH_LIBRARY})
endif()
