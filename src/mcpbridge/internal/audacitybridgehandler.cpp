/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity: A Digital Audio Editor
 */
#include "audacitybridgehandler.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>

#include "au3-import-export/ExportUtils.h"
#include "au3-project-rate/ProjectRate.h"
#include "au3-wave-track/WaveTrack.h"

#include "au3wrap/au3types.h"
#include "importexport/export/types/exporttypes.h"

#include "framework/global/progress.h"
#include "framework/global/translation.h"

using namespace au::mcpbridge;
namespace te = au::trackedit;

namespace {
//! Name of the WAV format in Au3Exporter::formatsList() (libsndfile's PCM plugin).
const std::string WAV_FORMAT = "WAV (Microsoft)";

//! libsndfile constants used by the PCM export plugin's options: the option id for
//! WAV encoding is the container type, and its value is the sample subtype.
constexpr int SF_FORMAT_WAV_ID = 0x010000;
constexpr int SF_FORMAT_PCM_16_ID = 0x0002;
constexpr int SF_FORMAT_PCM_24_ID = 0x0003;
constexpr int SF_FORMAT_FLOAT_ID = 0x0006;

QString trackTypeName(te::TrackType type)
{
    switch (type) {
    case te::TrackType::Mono: return QStringLiteral("mono");
    case te::TrackType::Stereo: return QStringLiteral("stereo");
    case te::TrackType::Label: return QStringLiteral("label");
    case te::TrackType::Undefined: break;
    }
    return QStringLiteral("unknown");
}

QString trackFormatName(te::TrackFormat format)
{
    switch (format) {
    case te::TrackFormat::Int16: return QStringLiteral("int16");
    case te::TrackFormat::Int24: return QStringLiteral("int24");
    case te::TrackFormat::Float32: return QStringLiteral("float32");
    case te::TrackFormat::Undefined: break;
    }
    return QStringLiteral("unknown");
}

double linearToDb(double v)
{
    return v > 0.0 ? 20.0 * std::log10(v) : -std::numeric_limits<double>::infinity();
}

QJsonValue dbOrNull(double linear)
{
    const double db = linearToDb(linear);
    return std::isfinite(db) ? QJsonValue(std::round(db * 1000.0) / 1000.0) : QJsonValue();
}

BridgeResult noProject()
{
    return BridgeResult::failure(errors::NoProject, QStringLiteral("no project is open in Audacity"));
}

BridgeResult failed(const QString& message)
{
    return BridgeResult::failure(errors::Failed, message);
}

BridgeResult notFound(const QString& message)
{
    return BridgeResult::failure(errors::NotFound, message);
}

te::TrackId toId(const QJsonValue& v)
{
    return static_cast<te::TrackId>(v.toDouble());
}
}

AudacityBridgeHandler::AudacityBridgeHandler(const muse::modularity::ContextPtr& ctx)
    : muse::Contextable(ctx)
{
}

BridgeResult AudacityBridgeHandler::handle(const QString& method, const QJsonObject& params)
{
    if (method == QLatin1String("ping")) {
        return ping();
    }

    //! Everything else needs an open project.
    if (!globalContext()->currentProject() || !globalContext()->currentTrackeditProject()) {
        return noProject();
    }

    if (method == QLatin1String("get_project")) {
        return getProject();
    }
    if (method == QLatin1String("list_tracks")) {
        return listTracks(params);
    }
    if (method == QLatin1String("import_wav")) {
        return importWav(params);
    }
    if (method == QLatin1String("set_selection")) {
        return setSelection(params);
    }
    if (method == QLatin1String("set_clip_gain")) {
        return setClipGain(params);
    }
    if (method == QLatin1String("add_label")) {
        return addLabel(params);
    }
    if (method == QLatin1String("undo")) {
        return undo();
    }
    if (method == QLatin1String("export_wav")) {
        return exportWav(params);
    }

    return BridgeResult::failure(errors::UnknownMethod, QStringLiteral("unknown method '%1'").arg(method));
}

BridgeResult AudacityBridgeHandler::ping() const
{
    QJsonObject r;
    r.insert(QStringLiteral("pong"), true);
    r.insert(QStringLiteral("protocol"), PROTOCOL_VERSION);
    r.insert(QStringLiteral("project_open"), globalContext()->currentProject() != nullptr);
    return BridgeResult::success(r);
}

QJsonObject AudacityBridgeHandler::selectionJson() const
{
    QJsonObject sel;
    sel.insert(QStringLiteral("empty"), selectionController()->timeSelectionIsEmpty());
    sel.insert(QStringLiteral("start"), selectionController()->dataSelectedStartTime().to_double());
    sel.insert(QStringLiteral("end"), selectionController()->dataSelectedEndTime().to_double());
    QJsonArray tracks;
    for (const te::TrackId id : selectionController()->selectedTracks()) {
        tracks.append(static_cast<double>(id));
    }
    sel.insert(QStringLiteral("track_ids"), tracks);
    return sel;
}

BridgeResult AudacityBridgeHandler::getProject() const
{
    const auto project = globalContext()->currentProject();
    const auto trackeditProject = globalContext()->currentTrackeditProject();
    const auto* au3Project = reinterpret_cast<const au3::Au3Project*>(project->au3ProjectPtr());

    QJsonObject r;
    r.insert(QStringLiteral("title"), project->displayName());
    r.insert(QStringLiteral("path"), project->path().toQString());
    r.insert(QStringLiteral("sample_rate"), au3Project ? ::ProjectRate::Get(*au3Project).GetRate() : 0.0);
    r.insert(QStringLiteral("duration"), trackeditProject->totalTime().to_double());
    r.insert(QStringLiteral("track_count"), static_cast<int>(trackeditProject->trackIdList().size()));
    r.insert(QStringLiteral("selection"), selectionJson());
    r.insert(QStringLiteral("can_undo"), trackeditInteraction()->canUndo());
    r.insert(QStringLiteral("undo_action"), projectHistory()->undoAvailable()
             ? projectHistory()->topMostUndoActionName().translated().toQString()
             : QString());
    return BridgeResult::success(r);
}

BridgeResult AudacityBridgeHandler::listTracks(const QJsonObject& params) const
{
    const bool includeItems = params.value(QStringLiteral("include_items")).toBool(true);
    const auto trackeditProject = globalContext()->currentTrackeditProject();

    QJsonArray tracks;
    for (const te::Track& track : trackeditProject->trackList()) {
        QJsonObject t;
        t.insert(QStringLiteral("id"), static_cast<double>(track.id));
        t.insert(QStringLiteral("title"), track.title.toQString());
        t.insert(QStringLiteral("type"), trackTypeName(track.type));
        t.insert(QStringLiteral("mute"), track.mute);
        t.insert(QStringLiteral("solo"), track.solo);
        if (track.type != te::TrackType::Label) {
            t.insert(QStringLiteral("channels"), track.type == te::TrackType::Stereo ? 2 : 1);
            t.insert(QStringLiteral("sample_rate"), static_cast<double>(track.rate));
            t.insert(QStringLiteral("format"), trackFormatName(track.format));
        }

        if (includeItems && track.type == te::TrackType::Label) {
            QJsonArray labels;
            for (const te::Label& label : trackeditProject->labelList(track.id)) {
                QJsonObject l;
                l.insert(QStringLiteral("id"), static_cast<double>(label.key.itemId));
                l.insert(QStringLiteral("text"), label.title.toQString());
                l.insert(QStringLiteral("start"), label.startTime);
                l.insert(QStringLiteral("end"), label.endTime);
                labels.append(l);
            }
            t.insert(QStringLiteral("labels"), labels);
        } else if (includeItems) {
            QJsonArray clips;
            for (const te::Clip& clip : trackeditProject->clipList(track.id)) {
                QJsonObject c;
                c.insert(QStringLiteral("id"), static_cast<double>(clip.key.itemId));
                c.insert(QStringLiteral("title"), clip.title.toQString());
                c.insert(QStringLiteral("start"), clip.startTime);
                c.insert(QStringLiteral("end"), clip.endTime);
                c.insert(QStringLiteral("stereo"), clip.stereo);

                const automation::AutomationPoints points = clipGainInteraction()->clipGainPoints(clip.key);
                c.insert(QStringLiteral("gain_points"), static_cast<int>(points.size()));
                bool uniform = true;
                for (const auto& p : points) {
                    if (std::abs(p.yValue - (points.empty() ? 1.0 : points.front().yValue)) > 1e-9) {
                        uniform = false;
                        break;
                    }
                }
                //! A clip without envelope points plays at unity gain.
                c.insert(QStringLiteral("gain_db"), uniform ? dbOrNull(points.empty() ? 1.0 : points.front().yValue) : QJsonValue());
                clips.append(c);
            }
            t.insert(QStringLiteral("clips"), clips);
        }

        tracks.append(t);
    }

    QJsonObject r;
    r.insert(QStringLiteral("tracks"), tracks);
    return BridgeResult::success(r);
}

BridgeResult AudacityBridgeHandler::importWav(const QJsonObject& params)
{
    const QString path = params.value(QStringLiteral("path")).toString();
    if (auto err = validateWavPath(path, QStringLiteral("path"))) {
        return BridgeResult::failure(err->code, err->message);
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return notFound(QStringLiteral("no such file: %1").arg(path));
    }
    if (!info.isReadable()) {
        return failed(QStringLiteral("file is not readable: %1").arg(path));
    }

    const auto project = globalContext()->currentProject();
    const std::vector<te::TrackId> before = globalContext()->currentTrackeditProject()->trackIdList();

    const muse::Ret ret = project->import(muse::io::path_t(path));
    if (!ret) {
        return failed(QStringLiteral("import failed: %1").arg(QString::fromStdString(ret.text())));
    }

    QJsonArray newTracks;
    for (const te::TrackId id : globalContext()->currentTrackeditProject()->trackIdList()) {
        if (std::find(before.begin(), before.end(), id) == before.end()) {
            newTracks.append(static_cast<double>(id));
        }
    }

    QJsonObject r;
    r.insert(QStringLiteral("imported"), path);
    r.insert(QStringLiteral("new_track_ids"), newTracks);
    return BridgeResult::success(r);
}

BridgeResult AudacityBridgeHandler::setSelection(const QJsonObject& params)
{
    const double start = params.value(QStringLiteral("start")).toDouble();
    const double end = params.value(QStringLiteral("end")).toDouble();
    if (!(end > start)) {
        return BridgeResult::failure(errors::InvalidParams, QStringLiteral("'end' must be greater than 'start'"));
    }

    const auto trackeditProject = globalContext()->currentTrackeditProject();
    te::TrackIdList trackIds;
    if (params.contains(QStringLiteral("track_ids"))) {
        const std::vector<te::TrackId> existing = trackeditProject->trackIdList();
        for (const QJsonValue& v : params.value(QStringLiteral("track_ids")).toArray()) {
            const te::TrackId id = toId(v);
            if (std::find(existing.begin(), existing.end(), id) == existing.end()) {
                return notFound(QStringLiteral("no track with id %1").arg(static_cast<qint64>(id)));
            }
            if (std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end()) {
                trackIds.push_back(id);
            }
        }
        if (trackIds.empty()) {
            return BridgeResult::failure(errors::InvalidParams, QStringLiteral("'track_ids' must not be empty"));
        }
    } else {
        trackIds = trackeditProject->trackIdList();
    }

    //! Same steps as ISelectionController::setSelectedAllAudioData(), limited to the requested tracks.
    selectionController()->resetSelectedClips();
    selectionController()->resetSelectedLabels();
    selectionController()->setSelectedTracks(trackIds, true);
    selectionController()->setDataSelectedStartTime(start, true);
    selectionController()->setDataSelectedEndTime(end, true);

    QJsonObject r;
    r.insert(QStringLiteral("selection"), selectionJson());
    return BridgeResult::success(r);
}

BridgeResult AudacityBridgeHandler::setClipGain(const QJsonObject& params)
{
    const te::ClipKey key { toId(params.value(QStringLiteral("track_id"))), toId(params.value(QStringLiteral("clip_id"))) };
    const double gainDb = params.value(QStringLiteral("gain_db")).toDouble();

    const auto trackeditProject = globalContext()->currentTrackeditProject();
    const std::optional<te::Track> track = trackeditProject->track(key.trackId);
    if (!track || track->type == te::TrackType::Label) {
        return notFound(QStringLiteral("no audio track with id %1").arg(static_cast<qint64>(key.trackId)));
    }
    const te::Clip clip = trackeditProject->clip(key);
    if (!clip.isValid()) {
        return notFound(QStringLiteral("no clip %1 on track %2").arg(static_cast<qint64>(key.itemId)).arg(static_cast<qint64>(key.trackId)));
    }

    const std::optional<automation::AutomationInfo> info = clipGainInteraction()->clipGainInfo(key);
    if (!info) {
        return failed(QStringLiteral("clip gain is not available for this clip"));
    }

    const double linear = std::pow(10.0, gainDb / 20.0);
    if (linear < info->minValue || linear > info->maxValue) {
        return BridgeResult::failure(errors::InvalidParams,
                                     QStringLiteral("gain must be between %1 and %2 dB for this clip")
                                     .arg(linearToDb(info->minValue), 0, 'f', 2).arg(linearToDb(info->maxValue), 0, 'f', 2));
    }

    //! Replace the clip's gain envelope with a flat one: drop existing points, then
    //! set the same value at both ends. Only the last step records an undo state,
    //! so the whole change is one undoable action.
    const int points = static_cast<int>(clipGainInteraction()->clipGainPoints(key).size());
    for (int i = points; i > 0; --i) {
        if (!clipGainInteraction()->removeClipGainPoint(key, i - 1, false)) {
            projectHistory()->rollbackState();
            return failed(QStringLiteral("could not clear the existing clip gain envelope"));
        }
    }

    const bool ok = clipGainInteraction()->setClipGainPoint(key, clip.startTime, linear, false)
                    && clipGainInteraction()->setClipGainPoint(key, clip.endTime, linear, true);
    if (!ok) {
        projectHistory()->rollbackState();
        return failed(QStringLiteral("could not set clip gain"));
    }

    QJsonObject r;
    r.insert(QStringLiteral("track_id"), static_cast<double>(key.trackId));
    r.insert(QStringLiteral("clip_id"), static_cast<double>(key.itemId));
    r.insert(QStringLiteral("gain_db"), dbOrNull(linear));
    r.insert(QStringLiteral("gain_linear"), linear);
    return BridgeResult::success(r);
}

BridgeResult AudacityBridgeHandler::addLabel(const QJsonObject& params)
{
    const double start = params.value(QStringLiteral("start")).toDouble();
    const double end = params.contains(QStringLiteral("end")) ? params.value(QStringLiteral("end")).toDouble() : start;
    if (end < start) {
        return BridgeResult::failure(errors::InvalidParams, QStringLiteral("'end' must not be before 'start'"));
    }
    const QString text = params.value(QStringLiteral("text")).toString();

    const auto trackeditProject = globalContext()->currentTrackeditProject();
    te::TrackId labelTrackId = te::INVALID_TRACK;
    bool createdTrack = false;

    if (params.contains(QStringLiteral("track_id"))) {
        labelTrackId = toId(params.value(QStringLiteral("track_id")));
        const std::optional<te::Track> track = trackeditProject->track(labelTrackId);
        if (!track || track->type != te::TrackType::Label) {
            return notFound(QStringLiteral("no label track with id %1").arg(static_cast<qint64>(labelTrackId)));
        }
    } else {
        for (const te::Track& track : trackeditProject->trackList()) {
            if (track.type == te::TrackType::Label) {
                labelTrackId = track.id;
                break;
            }
        }
        if (labelTrackId == te::INVALID_TRACK) {
            const muse::RetVal<te::TrackId> created = tracksInteraction()->newLabelTrack();
            if (!created.ret) {
                return failed(QStringLiteral("could not create a label track"));
            }
            labelTrackId = created.val;
            createdTrack = true;
        }
    }

    //! ILabelsInteraction does not record undo states itself; push exactly one at the end.
    const muse::RetVal<te::LabelKey> added = labelsInteraction()->addLabel(labelTrackId);
    bool ok = added.ret;
    if (ok) {
        ok = labelsInteraction()->stretchLabelRight(added.val, end, true)
             && labelsInteraction()->stretchLabelLeft(added.val, start, true);
    }
    if (ok && !text.isEmpty()) {
        ok = labelsInteraction()->changeLabelTitle(added.val, muse::String::fromQString(text));
    }
    if (!ok) {
        projectHistory()->rollbackState();
        trackeditProject->reload();
        return failed(QStringLiteral("could not add the label"));
    }

    projectHistory()->pushHistoryState(muse::trc("trackedit", "Label added"), muse::trc("trackedit", "Add label"));

    const te::Label label = trackeditProject->label(added.val);
    QJsonObject r;
    r.insert(QStringLiteral("track_id"), static_cast<double>(added.val.trackId));
    r.insert(QStringLiteral("label_id"), static_cast<double>(added.val.itemId));
    r.insert(QStringLiteral("start"), label.startTime);
    r.insert(QStringLiteral("end"), label.endTime);
    r.insert(QStringLiteral("text"), label.title.toQString());
    r.insert(QStringLiteral("created_label_track"), createdTrack);
    return BridgeResult::success(r);
}

BridgeResult AudacityBridgeHandler::undo()
{
    if (!trackeditInteraction()->canUndo()) {
        return failed(QStringLiteral("nothing to undo"));
    }

    const QString action = projectHistory()->topMostUndoActionName().translated().toQString();
    if (!trackeditInteraction()->undo()) {
        return failed(QStringLiteral("undo failed"));
    }

    QJsonObject r;
    r.insert(QStringLiteral("undone"), action);
    r.insert(QStringLiteral("can_undo"), trackeditInteraction()->canUndo());
    return BridgeResult::success(r);
}

BridgeResult AudacityBridgeHandler::exportWav(const QJsonObject& params)
{
    const QString path = params.value(QStringLiteral("path")).toString();
    if (auto err = validateWavPath(path, QStringLiteral("path"))) {
        return BridgeResult::failure(err->code, err->message);
    }

    const QFileInfo target(path);
    const QFileInfo parent(target.absolutePath());
    const bool overwrite = params.value(QStringLiteral("overwrite")).toBool(false);
    if (!parent.exists() || !parent.isDir()) {
        return notFound(QStringLiteral("output directory does not exist: %1").arg(parent.filePath()));
    }
    if (target.exists() || target.isSymLink()) {
        if (target.isDir()) {
            return BridgeResult::failure(errors::InvalidParams, QStringLiteral("'path' is a directory"));
        }
        if (!overwrite) {
            return failed(QStringLiteral("%1 already exists; pass overwrite=true to replace it").arg(path));
        }
    }

    const auto project = globalContext()->currentProject();
    auto* au3Project = reinterpret_cast<au3::Au3Project*>(project->au3ProjectPtr());
    if (!au3Project) {
        return noProject();
    }

    const bool selectionOnly = params.value(QStringLiteral("scope")).toString(QStringLiteral("project")) == QLatin1String("selection");
    if (selectionOnly && selectionController()->timeSelectionIsEmpty()) {
        return failed(QStringLiteral("the time selection is empty; call set_selection first"));
    }

    //! Decide channel count from the tracks that will actually be exported (respects
    //! mute/solo and, for scope=selection, track selection). Mirrors the audio.com
    //! upload rule: mono only when every exported track is mono and centred.
    const auto exportedTracks = ::ExportUtils::FindExportWaveTracks(::TrackList::Get(*au3Project), selectionOnly);
    if (exportedTracks.empty()) {
        return failed(selectionOnly ? QStringLiteral("no unmuted audio tracks are selected")
                      : QStringLiteral("the project has no unmuted audio tracks"));
    }
    const bool allMonoCentred = std::all_of(exportedTracks.begin(), exportedTracks.end(), [](const ::WaveTrack* t) {
        return t->NChannels() == 1 && t->GetPan() == 0;
    });

    const QString channelsParam = params.value(QStringLiteral("channels")).toString(QStringLiteral("auto"));
    int channels = allMonoCentred ? 1 : 2;
    if (channelsParam == QLatin1String("mono")) {
        channels = 1;
    } else if (channelsParam == QLatin1String("stereo")) {
        channels = 2;
    }

    const int projectRate = static_cast<int>(std::lround(::ProjectRate::Get(*au3Project).GetRate()));
    const int sampleRate = params.contains(QStringLiteral("sample_rate"))
                           ? params.value(QStringLiteral("sample_rate")).toInt()
                           : projectRate;

    const QString encoding = params.value(QStringLiteral("encoding")).toString(QStringLiteral("float32"));
    int subtype = SF_FORMAT_FLOAT_ID;
    if (encoding == QLatin1String("pcm24")) {
        subtype = SF_FORMAT_PCM_24_ID;
    } else if (encoding == QLatin1String("pcm16")) {
        subtype = SF_FORMAT_PCM_16_ID;
    }

    muse::ValMap encodingParam;
    encodingParam["id"] = muse::Val(SF_FORMAT_WAV_ID);
    encodingParam["value"] = muse::Val(subtype);

    using importexport::IExporter;
    IExporter::Options options;
    options[IExporter::OptionKey::Format] = muse::Val(WAV_FORMAT);
    options[IExporter::OptionKey::ProcessType] = muse::Val(selectionOnly
                                                           ? importexport::ExportProcessType::SELECTED_AUDIO
                                                           : importexport::ExportProcessType::FULL_PROJECT_AUDIO);
    options[IExporter::OptionKey::ExportChannelsType] = muse::Val(static_cast<int>(
                                                                      channels == 1
                                                                      ? importexport::ExportChannelsPref::ExportChannels::MONO
                                                                      : importexport::ExportChannelsPref::ExportChannels::STEREO));
    options[IExporter::OptionKey::ExportChannels] = muse::Val(channels);
    options[IExporter::OptionKey::ExportSampleRate] = muse::Val(sampleRate);
    options[IExporter::OptionKey::Parameters] = muse::Val(muse::ValList { muse::Val(encodingParam) });

    // Render beside the target so a failed export leaves any previous WAV intact.
    QTemporaryDir stagingDir(parent.absoluteFilePath() + QStringLiteral("/.audacity-mcp-export-XXXXXX"));
    if (!stagingDir.isValid()) {
        return failed(QStringLiteral("could not stage the export: %1").arg(stagingDir.errorString()));
    }
    const QString stagedPath = stagingDir.filePath(target.fileName());

    //! Passing a Progress keeps Au3Exporter from opening its own progress dialog.
    auto progress = std::make_shared<muse::Progress>();
    const muse::Ret ret = exporter()->exportData(muse::io::path_t(stagedPath), options, progress, project);
    if (!ret) {
        return failed(QStringLiteral("export failed: %1").arg(QString::fromStdString(ret.text())));
    }

    if (overwrite) {
        std::error_code error;
        std::filesystem::rename(std::filesystem::path(stagedPath.toStdU16String()),
                                std::filesystem::path(path.toStdU16String()), error);
        if (error) {
            return failed(QStringLiteral("could not replace %1: %2").arg(path, QString::fromStdString(error.message())));
        }
    } else {
        QFile stagedFile(stagedPath);
        if (!stagedFile.rename(path)) {
            return failed(QStringLiteral("could not place the export at %1: %2").arg(path, stagedFile.errorString()));
        }
    }

    QJsonObject r;
    r.insert(QStringLiteral("path"), path);
    r.insert(QStringLiteral("scope"), selectionOnly ? QStringLiteral("selection") : QStringLiteral("project"));
    r.insert(QStringLiteral("sample_rate"), sampleRate);
    r.insert(QStringLiteral("channels"), channels);
    r.insert(QStringLiteral("encoding"), encoding);
    if (selectionOnly) {
        r.insert(QStringLiteral("start"), selectionController()->dataSelectedStartTime().to_double());
        r.insert(QStringLiteral("end"), selectionController()->dataSelectedEndTime().to_double());
    }
    //! Audacity's export preference can skip leading silence; report it so clients can tell.
    r.insert(QStringLiteral("trim_blank_space"), exportConfiguration()->trimBlankSpace());
    return BridgeResult::success(r);
}
