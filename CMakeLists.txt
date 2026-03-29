cmake_minimum_required(VERSION 3.21)

project(SnipasteLikeCapturer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
add_compile_definitions(NOMINMAX)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

qt_standard_project_setup()

qt_add_executable(SnipasteLikeCapturer
    main.cpp
    coordinateconverter.h
    coordinateconverter.cpp
    screencapturer.h
    screencapturer_multi.cpp
    uiadetector.h
    uiadetector.cpp
)

target_link_libraries(SnipasteLikeCapturer
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Concurrent
        dwmapi
        ole32
        oleaut32
        uiautomationcore
)
