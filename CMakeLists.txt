cmake_minimum_required(VERSION 3.16)
project(rdbms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(RDBMS_SANITIZE "" CACHE STRING "Sanitizer to build with: address, undefined or thread")

if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /EHsc /utf-8 /MP /wd4324)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(RDBMS_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  elseif(RDBMS_SANITIZE STREQUAL "undefined")
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=undefined)
  elseif(RDBMS_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
  endif()
endif()

find_package(Threads REQUIRED)

add_library(rdbms STATIC
  src/common.cpp
  src/fs.cpp
  src/config.cpp
  src/page.cpp
  src/wal.cpp
  src/buffer.cpp
  src/btree.cpp
  src/txn.cpp
  src/value.cpp
  src/engine.cpp
  src/recovery.cpp
  src/parser.cpp
  src/executor.cpp
  src/session.cpp
  src/net.cpp
  src/server.cpp
  src/client.cpp)
target_include_directories(rdbms PUBLIC src)
target_link_libraries(rdbms PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(rdbms PUBLIC ws2_32)
endif()

add_executable(rdbms-server src/main.cpp)
target_link_libraries(rdbms-server PRIVATE rdbms)

add_executable(rdbms-bench tools/bench.cpp)
target_link_libraries(rdbms-bench PRIVATE rdbms)

add_executable(rdbms-crashsim tools/crashsim.cpp)
target_link_libraries(rdbms-crashsim PRIVATE rdbms)

enable_testing()
foreach(name storage wal btree txn recovery sql server)
  add_executable(test_${name} tests/test_${name}.cpp)
  target_include_directories(test_${name} PRIVATE tests)
  target_link_libraries(test_${name} PRIVATE rdbms)
  add_test(NAME ${name} COMMAND test_${name})
endforeach()
add_test(NAME crashsim COMMAND rdbms-crashsim --seeds 60)
