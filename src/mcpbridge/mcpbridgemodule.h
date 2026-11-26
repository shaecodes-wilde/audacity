/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <memory>

#include "framework/global/modularity/imodulesetup.h"

namespace au::mcpbridge {
class AudacityBridgeHandler;
class BridgeServer;

//! Opt-in local bridge for MCP clients (see docs/mcp-bridge.md).
//!
//! Nothing listens unless Audacity is started with AUDACITY_MCP_BRIDGE=1. The
//! bridge serves the first project window of the process.
class McpBridgeModule : public muse::modularity::IModuleSetup
{
public:
    std::string moduleName() const override;
    muse::modularity::IContextSetup* newContext(const muse::modularity::ContextPtr& ctx) const override;
};

class McpBridgeContext : public muse::modularity::IContextSetup
{
public:
    explicit McpBridgeContext(const muse::modularity::ContextPtr& ctx);
    ~McpBridgeContext() override;

    void onInit(const muse::IApplication::RunMode& mode) override;
    void onDeinit() override;

private:
    std::unique_ptr<AudacityBridgeHandler> m_handler;
    std::unique_ptr<BridgeServer> m_server;
};
}
