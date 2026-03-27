cmake_minimum_required(VERSION 3.16)

project(hfts LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hfts
  src/fiber.cpp
  src/memory.cpp
  src/scheduler.cpp
  src/thread.cpp
  src/worker.cpp
  src/osfiber_i.S
)

target_include_directories(hfts
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(hello_task
  example/hello_task.cpp
)

target_link_libraries(hello_task PRIVATE hfts)

add_executable(fractal
  example/fractal.cpp
)

target_link_libraries(fractal PRIVATE hfts)
