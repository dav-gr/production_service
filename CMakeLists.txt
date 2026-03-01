cmake_minimum_required(VERSION 3.21)
project(production_service VERSION 1.0 LANGUAGES CXX)

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

    # Core - Database
    core/db/db_service.cpp

    # Server
    server/tcp_server.cpp
    server/session_manager.cpp

    # Pipeline
    server/pipeline/entity_resolver.cpp
    server/pipeline/state_resolver.cpp
    server/pipeline/capability_engine.cpp
    server/pipeline/response_builder.cpp
    server/pipeline/action_executor.cpp

    # Pub/Sub
    pubsub/event_publisher.cpp
    pubsub/permanent_worker.cpp
)

set(HEADERS
    # Core
    core/db/types.h
    core/db/db_service.h

    # Server
    server/tcp_server.h
    server/protocol.h
    server/session_manager.h

    # Pipeline
    server/pipeline/entity_resolver.h
    server/pipeline/state_resolver.h
    server/pipeline/capability_engine.h
    server/pipeline/response_builder.h
    server/pipeline/action_executor.h

    # Pub/Sub
    pubsub/event_publisher.h
    pubsub/permanent_worker.h
)

# ============================================================================
# Create Executable
# ============================================================================

add_executable(production_service
    ${SOURCES}
    ${HEADERS}
)

target_include_directories(production_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ============================================================================
# PostgreSQL libpq (required for LISTEN/NOTIFY in pub/sub)
# ============================================================================
# Set PostgreSQL_ROOT via CMakeUserPresets.json or environment variable
# Example: set(PostgreSQL_ROOT "C:/Program Files/PostgreSQL/16")
set(PostgreSQL_ROOT "C:/local/PostgreSQL/18")
find_package(PostgreSQL QUIET)

if(PostgreSQL_FOUND)
message(STATUS "Found PostgreSQL: ${PostgreSQL_VERSION_STRING}")
message(STATUS "PostgreSQL include: ${PostgreSQL_INCLUDE_DIRS}")
message(STATUS "PostgreSQL libraries: ${PostgreSQL_LIBRARIES}")
    
target_include_directories(production_service PRIVATE ${PostgreSQL_INCLUDE_DIRS})
target_link_libraries(production_service PRIVATE ${PostgreSQL_LIBRARIES})
target_compile_definitions(production_service PRIVATE PUBSUB_USE_LIBPQ=1)
else()
    # Fallback: Try to find libpq manually if FindPostgreSQL fails
    # Common locations on Windows
    set(LIBPQ_SEARCH_PATHS
        "C:/Program Files/PostgreSQL/16"
        "C:/Program Files/PostgreSQL/15"
        "C:/Program Files/PostgreSQL/14"
        "$ENV{PGROOT}"
    )
    
    find_path(LIBPQ_INCLUDE_DIR libpq-fe.h
        HINTS ${LIBPQ_SEARCH_PATHS}
        PATH_SUFFIXES include
    )
    
    find_library(LIBPQ_LIBRARY
        NAMES libpq pq
        HINTS ${LIBPQ_SEARCH_PATHS}
        PATH_SUFFIXES lib
    )
    
    if(LIBPQ_INCLUDE_DIR AND LIBPQ_LIBRARY)
        message(STATUS "Found libpq manually:")
        message(STATUS "  Include: ${LIBPQ_INCLUDE_DIR}")
        message(STATUS "  Library: ${LIBPQ_LIBRARY}")
        
        target_include_directories(production_service PRIVATE ${LIBPQ_INCLUDE_DIR})
        target_link_libraries(production_service PRIVATE ${LIBPQ_LIBRARY})
        target_compile_definitions(production_service PRIVATE PUBSUB_USE_LIBPQ=1)
    else()
        message(WARNING "libpq not found! Pub/sub LISTEN/NOTIFY will not work.")
        message(WARNING "Set PostgreSQL_ROOT or install PostgreSQL.")
    endif()
endif()

target_link_libraries(production_service PRIVATE
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

add_custom_command(TARGET production_service POST_BUILD
    COMMAND "${QT6_BIN_DIR}/windeployqt.exe"
        --no-translations
        --no-system-d3d-compiler
        --no-opengl-sw
        "$<TARGET_FILE:production_service>"
    COMMENT "Running windeployqt..."
)

# ============================================================================
# Example Pub/Sub Client
# ============================================================================

add_executable(pubsub_example_client
    examples/main.cpp
    examples/client.cpp
    examples/client.h
)

target_include_directories(pubsub_example_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/examples
)

target_link_libraries(pubsub_example_client PRIVATE
    Qt6::Core
    Qt6::Network
)

add_custom_command(TARGET pubsub_example_client POST_BUILD
    COMMAND "${QT6_BIN_DIR}/windeployqt.exe"
        --no-translations
        --no-system-d3d-compiler
        --no-opengl-sw
        "$<TARGET_FILE:pubsub_example_client>"
    COMMENT "Running windeployqt for example client..."
)

# ============================================================================
# Configuration File
# ============================================================================

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.ini.example
    ${CMAKE_CURRENT_BINARY_DIR}/config.ini
    COPYONLY
)

# Copy capability rules to build directory
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/rules)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/rules/capability_rules.json
    ${CMAKE_CURRENT_BINARY_DIR}/rules/capability_rules.json
    COPYONLY
)

# ============================================================================
# Output Info
# ============================================================================

message(STATUS "")
message(STATUS "=== production_service Build Configuration ===")
message(STATUS "Platform: Windows (Visual Studio)")
message(STATUS "Qt Version: ${Qt6_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "Required Qt SQL Driver: QPSQL (PostgreSQL)")
message(STATUS "Ensure libpq.dll and qsqlpsql.dll are available!")
message(STATUS "===============================================")
