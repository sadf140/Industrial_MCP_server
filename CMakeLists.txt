cmake_minimum_required(VERSION 3.20)

project(industrial_mcp_server VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INDUSTRIAL_MCP_BUILD_TESTS "Build tests" ON)
option(INDUSTRIAL_MCP_WITH_OPCUA "Build with open62541pp OPC UA support" ON)

find_package(SQLite3 QUIET)

set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(third_party/json EXCLUDE_FROM_ALL)

if(INDUSTRIAL_MCP_WITH_OPCUA)
    set(UAPP_BUILD_TESTS OFF CACHE BOOL "Build open62541pp tests" FORCE)
    set(UAPP_BUILD_EXAMPLES OFF CACHE BOOL "Build open62541pp examples" FORCE)
    set(UAPP_BUILD_DOCUMENTATION OFF CACHE BOOL "Build open62541pp documentation" FORCE)
    set(UAPP_INTERNAL_OPEN62541 ON CACHE BOOL "Use open62541pp bundled open62541" FORCE)
    set(UA_BUILD_EXAMPLES OFF CACHE BOOL "Build open62541 examples" FORCE)
    set(UA_BUILD_TOOLS OFF CACHE BOOL "Build open62541 tools" FORCE)
    set(UA_BUILD_UNIT_TESTS OFF CACHE BOOL "Build open62541 tests" FORCE)
    set(UA_BUILD_INTEROP_TESTS OFF CACHE BOOL "Build open62541 interoperability tests" FORCE)
    set(UA_BUILD_FUZZING OFF CACHE BOOL "Build open62541 fuzzing targets" FORCE)
    set(UA_BUILD_OSS_FUZZ OFF CACHE BOOL "Build open62541 OSS-Fuzz targets" FORCE)
    set(UA_BUILD_FUZZING_CORPUS OFF CACHE BOOL "Build open62541 fuzzing corpus" FORCE)
    set(UA_ENABLE_METHODCALLS ON CACHE BOOL "Enable open62541 Method service set" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

    if(WIN32 AND MINGW AND DEFINED ENV{SHELL})
        set(_INDUSTRIAL_MCP_SAVED_SHELL "$ENV{SHELL}")
        unset(ENV{SHELL})
    endif()
    add_subdirectory(third_party/open62541pp EXCLUDE_FROM_ALL)
    if(DEFINED _INDUSTRIAL_MCP_SAVED_SHELL)
        set(ENV{SHELL} "${_INDUSTRIAL_MCP_SAVED_SHELL}")
        unset(_INDUSTRIAL_MCP_SAVED_SHELL)
    endif()
endif()

add_library(industrial_mcp_core
    src/alarm/alarm_store.cpp
    src/connector/industrial_connector.cpp
    src/connector/opcua_client.cpp
    src/device/device_state_cache.cpp
    src/diagnosis/diagnostics.cpp
    src/mcp/config.cpp
    src/mcp/json.cpp
    src/mcp/mcp_server.cpp
    src/observability/audit_logger.cpp
    src/observability/http_server.cpp
    src/observability/observability.cpp
    src/reliability/device_connection_manager.cpp
    src/storage/storage_backend.cpp
)

target_include_directories(industrial_mcp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(industrial_mcp_core PUBLIC nlohmann_json::nlohmann_json)

if(INDUSTRIAL_MCP_WITH_OPCUA)
    target_link_libraries(industrial_mcp_core PUBLIC open62541pp::open62541pp)
    target_compile_definitions(industrial_mcp_core PUBLIC INDUSTRIAL_MCP_WITH_OPCUA=1)
endif()

if(SQLite3_FOUND)
    target_link_libraries(industrial_mcp_core PUBLIC SQLite::SQLite3)
    target_compile_definitions(industrial_mcp_core PUBLIC INDUSTRIAL_MCP_WITH_SQLITE=1)
endif()

if(WIN32)
    target_link_libraries(industrial_mcp_core PUBLIC ws2_32)
endif()

add_executable(industrial-mcp-server src/main.cpp)
target_link_libraries(industrial-mcp-server PRIVATE industrial_mcp_core)

if(INDUSTRIAL_MCP_WITH_OPCUA)
    add_executable(opcua-sim-server src/connector/opcua_sim_server.cpp)
    target_link_libraries(opcua-sim-server PRIVATE open62541pp::open62541pp)
endif()

if(INDUSTRIAL_MCP_BUILD_TESTS)
    enable_testing()
    add_executable(industrial_mcp_core_tests tests/test_core.cpp)
    target_link_libraries(industrial_mcp_core_tests PRIVATE industrial_mcp_core)
    add_test(NAME industrial_mcp_tests COMMAND industrial_mcp_core_tests)

    add_executable(industrial_mcp_unit_tests tests/unit/test_unit.cpp)
    target_link_libraries(industrial_mcp_unit_tests PRIVATE industrial_mcp_core)
    add_test(NAME industrial_mcp_unit_tests COMMAND industrial_mcp_unit_tests)

    add_executable(industrial_mcp_integration_tests tests/integration/test_integration.cpp)
    target_link_libraries(industrial_mcp_integration_tests PRIVATE industrial_mcp_core)
    add_test(NAME industrial_mcp_integration_tests COMMAND industrial_mcp_integration_tests)

    add_executable(industrial_mcp_fault_injection_tests tests/fault_injection/test_fault_injection.cpp)
    target_link_libraries(industrial_mcp_fault_injection_tests PRIVATE industrial_mcp_core)
    add_test(NAME industrial_mcp_fault_injection_tests COMMAND industrial_mcp_fault_injection_tests)

    add_custom_target(industrial_mcp_tests
        DEPENDS
            industrial_mcp_core_tests
            industrial_mcp_unit_tests
            industrial_mcp_integration_tests
            industrial_mcp_fault_injection_tests
    )
endif()
