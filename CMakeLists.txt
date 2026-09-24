cmake_minimum_required(VERSION 3.25)

project(Ultraviolent
    VERSION 0.0.0
    DESCRIPTION "Clean-room virtual hardware platform for SGI-class systems"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Warnings)
include(Sanitizers)

add_library(ultraviolent_core
    src/core/virtual_time.cpp
)

target_include_directories(ultraviolent_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

ultraviolent_enable_warnings(ultraviolent_core)
ultraviolent_enable_sanitizers(ultraviolent_core)

add_executable(ultraviolent
    src/main.cpp
)

target_link_libraries(ultraviolent PRIVATE ultraviolent_core)
ultraviolent_enable_warnings(ultraviolent)
ultraviolent_enable_sanitizers(ultraviolent)

include(CTest)

if(BUILD_TESTING)
    add_executable(core_test tests/core/core_test.cpp)
    target_link_libraries(core_test PRIVATE ultraviolent_core)
    ultraviolent_enable_warnings(core_test)
    ultraviolent_enable_sanitizers(core_test)
    add_test(NAME core_test COMMAND core_test)
endif()