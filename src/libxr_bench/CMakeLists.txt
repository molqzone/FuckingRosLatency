cmake_minimum_required(VERSION 3.10)
project(libxr_bench)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-g -O3 -Wall)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LIBXR_LOG_OUTPUT_LEVEL 2)
add_subdirectory(libxr)

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PUBLIC xr)

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<TARGET_PROPERTY:xr,INTERFACE_INCLUDE_DIRECTORIES>)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
