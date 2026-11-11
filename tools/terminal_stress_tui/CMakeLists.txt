cmake_minimum_required(VERSION 3.22)

project(SpanTuiTerminalStressTui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Test)

add_library(terminal_stress_tui_core STATIC
    protocol_stages.cpp
    protocol_stages.h
    stress_tui.cpp
    stress_tui.h
)
target_include_directories(terminal_stress_tui_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(terminal_stress_tui_core PUBLIC Qt6::Core)

add_executable(terminal_stress_tui main.cpp)
target_link_libraries(terminal_stress_tui PRIVATE terminal_stress_tui_core)
set_target_properties(terminal_stress_tui PROPERTIES OUTPUT_NAME "terminal-stress-tui")

foreach(target terminal_stress_tui_core terminal_stress_tui)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(terminal_stress_tui_test tests/stress_tui_test.cpp)
    target_link_libraries(terminal_stress_tui_test PRIVATE terminal_stress_tui_core Qt6::Test)
    add_test(NAME terminal_stress_tui_test COMMAND terminal_stress_tui_test)
endif()
