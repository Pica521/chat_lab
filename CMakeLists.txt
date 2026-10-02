cmake_minimum_required(VERSION 3.17)
project(chat_lab CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-ferror-limit=5)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-fmax-errors=5)
endif()

option(ENABLE_ASAN "Enable AddressSanitizer and UBSan" OFF)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(chat_core
  common/net.cpp
  common/protocol.cpp
)

target_include_directories(chat_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(chat_core PUBLIC
  Threads::Threads
  OpenSSL::Crypto
  nlohmann_json::nlohmann_json
)

add_executable(chat_server server/main.cpp)
target_link_libraries(chat_server PRIVATE chat_core)

add_executable(chat_client client/main.cpp)
target_link_libraries(chat_client PRIVATE chat_core)

enable_testing()
add_executable(protocol_test tests/protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE chat_core)
add_test(NAME protocol_test COMMAND protocol_test)