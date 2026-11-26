/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include "framework/global/modularity/ioc.h"

#include "context/iglobalcontext.h"
#include "trackedit/itrackeditinteraction.h"
#include "trackedit/itracksinteraction.h"
#include "trackedit/ilabelsinteraction.h"
#include "trackedit/iselectioncontroller.h"
#include "trackedit/iprojecthistory.h"
#include "automation/iclipgaininteraction.h"
#include "importexport/export/iexporter.h"
#include "importexport/export/iexportconfiguration.h"

#include "bridgeserver.h"

namespace au::mcpbridge {
//! Maps the bridge's allowlisted methods onto existing Audacity interfaces.
//! Runs on the GUI thread (the BridgeServer lives there); params are already
//! validated against bridgeprotocol's MethodSpecs.
class AudacityBridgeHandler : public IBridgeHandler, public muse::Contextable
{
    muse::GlobalInject<importexport::IExportConfiguration> exportConfiguration;
    muse::ContextInject<context::IGlobalContext> globalContext{ this };
    muse::ContextInject<trackedit::ITrackeditInteraction> trackeditInteraction{ this };
    muse::ContextInject<trackedit::ITracksInteraction> tracksInteraction{ this };
    muse::ContextInject<trackedit::ILabelsInteraction> labelsInteraction{ this };
    muse::ContextInject<trackedit::ISelectionController> selectionController{ this };
    muse::ContextInject<trackedit::IProjectHistory> projectHistory{ this };
    muse::ContextInject<automation::IClipGainInteraction> clipGainInteraction{ this };
    muse::ContextInject<importexport::IExporter> exporter{ this };

public:
    explicit AudacityBridgeHandler(const muse::modularity::ContextPtr& ctx);

    BridgeResult handle(const QString& method, const QJsonObject& params) override;

private:
    BridgeResult ping() const;
    BridgeResult getProject() const;
    BridgeResult listTracks(const QJsonObject& params) const;
    BridgeResult importWav(const QJsonObject& params);
    BridgeResult setSelection(const QJsonObject& params);
    BridgeResult setClipGain(const QJsonObject& params);
    BridgeResult addLabel(const QJsonObject& params);
    BridgeResult undo();
    BridgeResult exportWav(const QJsonObject& params);

    QJsonObject selectionJson() const;
};
}
