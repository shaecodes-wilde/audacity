#
# Audacity: A Digital Audio Editor
#
set(MODULE_TEST mcpbridge_tests)

set(MODULE_TEST_SRC
    ${CMAKE_CURRENT_LIST_DIR}/bridgeserver_tests.cpp
)

set(MODULE_TEST_LINK
    mcpbridge
    Qt6::Network
)

set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)
