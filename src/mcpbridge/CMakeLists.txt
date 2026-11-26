#
# Audacity: A Digital Audio Editor
#
# Opt-in, token-authenticated local bridge for MCP clients. See docs/mcp-bridge.md.
#
declare_module(mcpbridge)

find_package(Qt6 REQUIRED COMPONENTS Network)

set(MODULE_SRC
    ${CMAKE_CURRENT_LIST_DIR}/mcpbridgemodule.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mcpbridgemodule.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/bridgeprotocol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/bridgeprotocol.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/bridgeserver.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/bridgeserver.h
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacitybridgehandler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/internal/audacitybridgehandler.h
)

# The handler mixes AU3 headers (global TrackList, Track, ...) with au::trackedit
# names; keep each file in its own translation unit.
set(MODULE_USE_UNITY OFF)
set(MODULE_LINK au3wrap Qt6::Network)

setup_module()

if (AU_BUILD_MCPBRIDGE_TESTS)
    add_subdirectory(tests)
endif()
