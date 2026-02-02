cmake_minimum_required(VERSION 3.20)
project(PCcUpdater LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

add_executable(pc_updater src/main.cpp)
set_target_properties(pc_updater PROPERTIES OUTPUT_NAME "pccontroller")

target_include_directories(pc_updater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(pc_updater PRIVATE CURL::libcurl)
