cmake_minimum_required(VERSION 3.20)

project(wh_repeater VERSION 0.1.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavdevice
    libavcodec
    libavfilter
    libavutil
    libswresample
    libswscale
)
pkg_check_modules(GSTREAMER QUIET IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(WH_REPEATER_BUILD_GATEWAY_TOOLS "Build TS gateway validation tools" ON)

add_executable(wh-repeater
    src/main.cpp
    src/api_server.cpp
    src/config.cpp
    src/daemon.cpp
    src/gateway_status_client.cpp
    src/nim_controller.cpp
    src/pc_gateway_input.cpp
    src/scan_scheduler.cpp
    src/signal_arbitrator.cpp
    src/ts_router.cpp
    src/ts_gateway_sink.cpp
    src/pluto_sink.cpp
    src/pluto_mqtt_status.cpp
    src/ident.cpp
    src/hardware_ptt.cpp
    src/i2c_bus_lock.cpp
    src/gstreamer_backend.cpp
    src/media_process.cpp
    src/media_pipeline.cpp
    src/sd1_controller.cpp
)

target_include_directories(wh-repeater PRIVATE include)
target_link_libraries(wh-repeater PRIVATE PkgConfig::FFMPEG)
if(GSTREAMER_FOUND)
    target_compile_definitions(wh-repeater PRIVATE WH_REPEATER_HAVE_GSTREAMER=1)
    target_link_libraries(wh-repeater PRIVATE PkgConfig::GSTREAMER)
endif()

if(WH_REPEATER_BUILD_GATEWAY_TOOLS)
    add_executable(ts-gateway-inspect
        tools/ts_gateway_inspect.cpp
    )
endif()

if(MSVC)
    target_compile_options(wh-repeater PRIVATE /W4)
    if(WH_REPEATER_BUILD_GATEWAY_TOOLS)
        target_compile_options(ts-gateway-inspect PRIVATE /W4)
    endif()
else()
    target_compile_options(wh-repeater PRIVATE -Wall -Wextra -Wpedantic)
    if(WH_REPEATER_BUILD_GATEWAY_TOOLS)
        target_compile_options(ts-gateway-inspect PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
