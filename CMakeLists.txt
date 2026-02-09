cmake_minimum_required(VERSION 3.21)
project(PostProdMiddleware VERSION 1.0 LANGUAGES CXX)

# ============================================================================
# Windows / Visual Studio Configuration
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt settings
set(CMAKE_AUTOMOC ON)

if(MSVC)
    add_compile_options(/W4 /utf-8 /MP)
endif()

# ============================================================================
# Find Qt6
# ============================================================================
# Set Qt6_DIR via CMakeUserPresets.json or environment variable
# Example: set(Qt6_DIR "C:/Qt/6.10.1/msvc2022_64/lib/cmake/Qt6")
set(Qt6_DIR "C:/Qt/6.10.1/msvc2022_64/lib/cmake/Qt6")
find_package(Qt6 REQUIRED COMPONENTS 
    Core 
    Network
    Sql 
    Concurrent
)

message(STATUS "Found Qt6 version: ${Qt6_VERSION}")
message(STATUS "Qt6 location: ${Qt6_DIR}")

# ============================================================================
# Source Files
# ============================================================================

set(SOURCES
    main.cpp
    
    # Core - Database (from submodule/shared)
    core/db/db_service.cpp
    
    # Server
    server/tcp_server.cpp
    server/request_handler.cpp
)

set(HEADERS
    # Core - Database
    core/db/types.h
    core/db/db_service.h
    
    # Server
    server/tcp_server.h
    server/request_handler.h
    server/protocol.h
)

# ============================================================================
# Create Executable
# ============================================================================

add_executable(PostProdMiddleware
    ${SOURCES}
    ${HEADERS}
)

target_include_directories(PostProdMiddleware PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(PostProdMiddleware PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Concurrent
)

# ============================================================================
# Windows Deployment
# ============================================================================

get_target_property(QT6_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT6_BIN_DIR "${QT6_QMAKE_EXECUTABLE}" DIRECTORY)

add_custom_command(TARGET PostProdMiddleware POST_BUILD
    COMMAND "${QT6_BIN_DIR}/windeployqt.exe"
        --no-translations
        --no-system-d3d-compiler
        --no-opengl-sw
        "$<TARGET_FILE:PostProdMiddleware>"
    COMMENT "Running windeployqt..."
)

# ============================================================================
# Configuration File
# ============================================================================

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.ini.example
    ${CMAKE_CURRENT_BINARY_DIR}/config.ini
    COPYONLY
)

# ============================================================================
# Output Info
# ============================================================================

message(STATUS "")
message(STATUS "=== PostProdMiddleware Build Configuration ===")
message(STATUS "Platform: Windows (Visual Studio)")
message(STATUS "Qt Version: ${Qt6_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "Required Qt SQL Driver: QPSQL (PostgreSQL)")
message(STATUS "Ensure libpq.dll and qsqlpsql.dll are available!")
message(STATUS "===============================================")
