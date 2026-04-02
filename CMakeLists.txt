cmake_minimum_required(VERSION 3.21)

project(SnipasteLikeCapturer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
# add_compile_definitions(NOMINMAX)
if(NOT MSVC)
    message(FATAL_ERROR "This project must be configured with an MSVC Qt kit, for example Qt 6.8.3 (msvc2022_64).")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

qt_standard_project_setup()

qt_add_executable(SnipasteLikeCapturer
    src/main.cpp
    include/coordinateconverter.h
    src/coordinateconverter.cpp
    include/screencapturer.h
    src/screencapturer_multi.cpp
    include/uiadetector.h
    src/uiadetector.cpp
    src/uiadetector_impl.cpp
)

set_source_files_properties(src/uiadetector_impl.cpp PROPERTIES HEADER_FILE_ONLY TRUE)

target_include_directories(SnipasteLikeCapturer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

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
