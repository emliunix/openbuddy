cmake_minimum_required(VERSION 3.20)
project(openbuddy CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)

# nlohmann-json installed via brew
find_package(nlohmann_json REQUIRED)

# Asio (header-only, installed via brew)
find_path(ASIO_INCLUDE_DIR asio.hpp PATHS /opt/homebrew/include /usr/local/include)
if(NOT ASIO_INCLUDE_DIR)
    message(FATAL_ERROR "asio.hpp not found. Install with: brew install asio")
endif()

file(GLOB SPECIES_SRC src/species/*.cpp)

add_executable(buddy
    src/main.cpp
    src/renderer.cpp
    src/buddy_renderer.cpp
    src/session.cpp
    src/session_renderer.cpp
    src/stats.cpp
    src/persistence.cpp
    src/mock_transport.cpp
    src/asio_transport.cpp
    ${SPECIES_SRC}
)

target_include_directories(buddy PRIVATE src ${ASIO_INCLUDE_DIR})

target_link_libraries(buddy PRIVATE SDL3::SDL3 nlohmann_json::nlohmann_json)

if (APPLE)
    target_link_libraries(buddy PRIVATE "-framework Cocoa")
endif()

# Unit tests
add_executable(test_protocol_types
    src/test_protocol_types.cpp
)
target_include_directories(test_protocol_types PRIVATE src)
target_link_libraries(test_protocol_types PRIVATE nlohmann_json::nlohmann_json)
