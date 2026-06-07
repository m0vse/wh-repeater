cmake_minimum_required(VERSION 3.20)

project(wh_repeater VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(wh-repeater
    src/main.cpp
    src/config.cpp
    src/daemon.cpp
    src/nim_controller.cpp
    src/scan_scheduler.cpp
    src/signal_arbitrator.cpp
    src/ts_router.cpp
    src/pluto_sink.cpp
    src/ident.cpp
)

target_include_directories(wh-repeater PRIVATE include)

if(MSVC)
    target_compile_options(wh-repeater PRIVATE /W4)
else()
    target_compile_options(wh-repeater PRIVATE -Wall -Wextra -Wpedantic)
endif()
