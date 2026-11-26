/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity: A Digital Audio Editor
 */
#include "mcpbridgemodule.h"

#include "framework/global/log.h"

#include "internal/audacitybridgehandler.h"
#include "internal/bridgeserver.h"

using namespace au::mcpbridge;

namespace {
const std::string MODULE_NAME("mcpbridge");

//! One bridge per process, owned by the first context that starts it.
bool s_bridgeRunning = false;

bool bridgeRequested()
{
    return qEnvironmentVariable("AUDACITY_MCP_BRIDGE") == QLatin1String("1");
}
}

std::string McpBridgeModule::moduleName() const
{
    return MODULE_NAME;
}

muse::modularity::IContextSetup* McpBridgeModule::newContext(const muse::modularity::ContextPtr& ctx) const
{
    return new McpBridgeContext(ctx);
}

McpBridgeContext::McpBridgeContext(const muse::modularity::ContextPtr& ctx)
    : muse::modularity::IContextSetup(ctx)
{
}

McpBridgeContext::~McpBridgeContext()
{
    onDeinit();
}

void McpBridgeContext::onInit(const muse::IApplication::RunMode& mode)
{
    if (mode != muse::IApplication::RunMode::GuiApp || !bridgeRequested() || s_bridgeRunning) {
        return;
    }

    m_handler = std::make_unique<AudacityBridgeHandler>(iocContext());
    m_server = std::make_unique<BridgeServer>(m_handler.get());

    QString error;
    const QString dir = BridgeServer::defaultDirectory();
    if (!m_server->start(dir, &error)) {
        LOGE() << "MCP bridge not started: " << error.toStdString();
        m_server.reset();
        m_handler.reset();
        return;
    }

    s_bridgeRunning = true;
    LOGI() << "MCP bridge listening on " << m_server->socketPath().toStdString();
}

void McpBridgeContext::onDeinit()
{
    if (!m_server) {
        return;
    }
    m_server->stop();
    m_server.reset();
    m_handler.reset();
    s_bridgeRunning = false;
}
