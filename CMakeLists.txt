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
option(WH_REPEATER_BUILD_TESTS "Build unit tests" ON)

set(WH_REPEATER_COMMON_SOURCES
    src/main.cpp
    src/api_server.cpp
    src/config.cpp
    src/daemon.cpp
    src/gateway_status_client.cpp
    src/hardware_ptt.cpp
    src/ident.cpp
)

set(WH_REPEATER_PI_GATEWAY_SOURCES
    ${WH_REPEATER_COMMON_SOURCES}
    src/i2c_bus_lock.cpp
    src/nim_controller.cpp
    src/scan_scheduler.cpp
    src/signal_arbitrator.cpp
    src/ts_gateway_sink.cpp
    src/ts_router.cpp
)

set(WH_REPEATER_PC_GATEWAY_SOURCES
    ${WH_REPEATER_COMMON_SOURCES}
    src/pc_gateway_input.cpp
    src/pluto_sink.cpp
    src/pluto_mqtt_status.cpp
    src/gstreamer_backend.cpp
    src/media_process.cpp
    src/media_timing.cpp
    src/media_pipeline.cpp
)

set(WH_REPEATER_LEGACY_SOURCES
    ${WH_REPEATER_COMMON_SOURCES}
    src/i2c_bus_lock.cpp
    src/nim_controller.cpp
    src/pc_gateway_input.cpp
    src/scan_scheduler.cpp
    src/signal_arbitrator.cpp
    src/ts_gateway_sink.cpp
    src/ts_router.cpp
    src/pluto_sink.cpp
    src/pluto_mqtt_status.cpp
    src/gstreamer_backend.cpp
    src/media_process.cpp
    src/media_timing.cpp
    src/media_pipeline.cpp
)

function(configure_wh_target target)
    target_include_directories(${target} PRIVATE include)
    target_link_libraries(${target} PRIVATE PkgConfig::FFMPEG)
    if(GSTREAMER_FOUND)
        target_compile_definitions(${target} PRIVATE WH_REPEATER_HAVE_GSTREAMER=1)
        target_link_libraries(${target} PRIVATE PkgConfig::GSTREAMER)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_executable(wh-pi-gateway ${WH_REPEATER_PI_GATEWAY_SOURCES})
target_compile_definitions(wh-pi-gateway PRIVATE WH_REPEATER_PI_GATEWAY_ONLY=1)
configure_wh_target(wh-pi-gateway)

add_executable(wh-pc-gateway ${WH_REPEATER_PC_GATEWAY_SOURCES})
target_compile_definitions(wh-pc-gateway PRIVATE WH_REPEATER_PC_GATEWAY_ONLY=1)
configure_wh_target(wh-pc-gateway)

add_executable(wh-repeater ${WH_REPEATER_LEGACY_SOURCES})
configure_wh_target(wh-repeater)

if(WH_REPEATER_BUILD_GATEWAY_TOOLS)
    add_executable(ts-gateway-inspect
        tools/ts_gateway_inspect.cpp
    )
endif()

if(WH_REPEATER_BUILD_GATEWAY_TOOLS)
    if(MSVC)
        target_compile_options(ts-gateway-inspect PRIVATE /W4)
    else()
        target_compile_options(ts-gateway-inspect PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(WH_REPEATER_BUILD_TESTS)
    enable_testing()

    add_executable(config-tests
        tests/config_tests.cpp
        src/config.cpp
    )
    target_include_directories(config-tests PRIVATE include)
    if(MSVC)
        target_compile_options(config-tests PRIVATE /W4)
    else()
        target_compile_options(config-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME config-tests COMMAND config-tests)

    add_executable(media-timing-tests
        tests/media_timing_tests.cpp
        src/media_timing.cpp
    )
    target_include_directories(media-timing-tests PRIVATE include)
    if(MSVC)
        target_compile_options(media-timing-tests PRIVATE /W4)
    else()
        target_compile_options(media-timing-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME media-timing-tests COMMAND media-timing-tests)
endif()
