/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity: A Digital Audio Editor
 */
#include "bridgeprotocol.h"

#include <cmath>

#include <QDir>
#include <QJsonArray>

using namespace au::mcpbridge;

namespace {
constexpr double MAX_ID = 9007199254740991.0; // 2^53 - 1, the largest integer a JSON number carries exactly
constexpr double MAX_TIME_SECS = 60.0 * 60.0 * 24.0 * 7.0; // a week of audio is far beyond any real project
constexpr int MAX_PATH_CHARS = 4096;

ParamSpec number(const QString& name, bool required, double min, double max)
{
    ParamSpec p;
    p.name = name;
    p.type = ParamSpec::Type::Number;
    p.required = required;
    p.min = min;
    p.max = max;
    return p;
}

ParamSpec integer(const QString& name, bool required, double min, double max)
{
    ParamSpec p = number(name, required, min, max);
    p.type = ParamSpec::Type::Integer;
    return p;
}

ParamSpec string(const QString& name, bool required, int maxLength)
{
    ParamSpec p;
    p.name = name;
    p.type = ParamSpec::Type::String;
    p.required = required;
    p.maxLength = maxLength;
    return p;
}

ParamSpec boolean(const QString& name)
{
    ParamSpec p;
    p.name = name;
    p.type = ParamSpec::Type::Bool;
    return p;
}

ParamSpec enumeration(const QString& name, const QStringList& values)
{
    ParamSpec p;
    p.name = name;
    p.type = ParamSpec::Type::Enum;
    p.enumValues = values;
    return p;
}

ParamSpec integerArray(const QString& name, int maxLength)
{
    ParamSpec p;
    p.name = name;
    p.type = ParamSpec::Type::IntegerArray;
    p.min = 0;
    p.max = MAX_ID;
    p.maxLength = maxLength;
    return p;
}

BridgeError invalid(const QString& message)
{
    return { errors::InvalidParams, message };
}

bool isIntegral(double v)
{
    return std::isfinite(v) && std::floor(v) == v;
}

std::optional<BridgeError> checkValue(const ParamSpec& spec, const QJsonValue& value)
{
    const QString& n = spec.name;
    switch (spec.type) {
    case ParamSpec::Type::Number:
    case ParamSpec::Type::Integer: {
        if (!value.isDouble()) {
            return invalid(QStringLiteral("'%1' must be a number").arg(n));
        }
        const double v = value.toDouble();
        if (!std::isfinite(v)) {
            return invalid(QStringLiteral("'%1' must be finite").arg(n));
        }
        if (spec.type == ParamSpec::Type::Integer && !isIntegral(v)) {
            return invalid(QStringLiteral("'%1' must be an integer").arg(n));
        }
        if (v < spec.min || v > spec.max) {
            return invalid(QStringLiteral("'%1' must be between %2 and %3").arg(n).arg(spec.min, 0, 'g', 16).arg(spec.max, 0, 'g', 16));
        }
        return std::nullopt;
    }
    case ParamSpec::Type::String: {
        if (!value.isString()) {
            return invalid(QStringLiteral("'%1' must be a string").arg(n));
        }
        const QString s = value.toString();
        if (s.isEmpty()) {
            return invalid(QStringLiteral("'%1' must not be empty").arg(n));
        }
        if (s.size() > spec.maxLength) {
            return invalid(QStringLiteral("'%1' is longer than %2 characters").arg(n).arg(spec.maxLength));
        }
        if (s.contains(QChar(0))) {
            return invalid(QStringLiteral("'%1' must not contain NUL characters").arg(n));
        }
        return std::nullopt;
    }
    case ParamSpec::Type::Bool:
        if (!value.isBool()) {
            return invalid(QStringLiteral("'%1' must be true or false").arg(n));
        }
        return std::nullopt;
    case ParamSpec::Type::Enum:
        if (!value.isString() || !spec.enumValues.contains(value.toString())) {
            return invalid(QStringLiteral("'%1' must be one of: %2").arg(n, spec.enumValues.join(QStringLiteral(", "))));
        }
        return std::nullopt;
    case ParamSpec::Type::IntegerArray: {
        if (!value.isArray()) {
            return invalid(QStringLiteral("'%1' must be an array of integers").arg(n));
        }
        const QJsonArray arr = value.toArray();
        if (arr.size() > spec.maxLength) {
            return invalid(QStringLiteral("'%1' has more than %2 entries").arg(n).arg(spec.maxLength));
        }
        for (const QJsonValue& item : arr) {
            const double v = item.toDouble(-1.0);
            if (!item.isDouble() || !isIntegral(v) || v < spec.min || v > spec.max) {
                return invalid(QStringLiteral("'%1' must contain only non-negative integers").arg(n));
            }
        }
        return std::nullopt;
    }
    }
    return invalid(QStringLiteral("'%1' has an unsupported type").arg(n));
}
}

const std::vector<MethodSpec>& au::mcpbridge::methodSpecs()
{
    static const std::vector<MethodSpec> specs = {
        { QStringLiteral("ping"), {} },
        { QStringLiteral("get_project"), {} },
        { QStringLiteral("list_tracks"), { boolean(QStringLiteral("include_items")) } },
        { QStringLiteral("import_wav"), { string(QStringLiteral("path"), true, MAX_PATH_CHARS) } },
        { QStringLiteral("set_selection"), {
              number(QStringLiteral("start"), true, 0.0, MAX_TIME_SECS),
              number(QStringLiteral("end"), true, 0.0, MAX_TIME_SECS),
              integerArray(QStringLiteral("track_ids"), 1024),
          } },
        { QStringLiteral("set_clip_gain"), {
              integer(QStringLiteral("track_id"), true, 0, MAX_ID),
              integer(QStringLiteral("clip_id"), true, 0, MAX_ID),
              number(QStringLiteral("gain_db"), true, -60.0, 6.0),
          } },
        { QStringLiteral("add_label"), {
              number(QStringLiteral("start"), true, 0.0, MAX_TIME_SECS),
              number(QStringLiteral("end"), false, 0.0, MAX_TIME_SECS),
              string(QStringLiteral("text"), false, 1000),
              integer(QStringLiteral("track_id"), false, 0, MAX_ID),
          } },
        { QStringLiteral("undo"), {} },
        { QStringLiteral("export_wav"), {
              string(QStringLiteral("path"), true, MAX_PATH_CHARS),
              enumeration(QStringLiteral("scope"), { QStringLiteral("project"), QStringLiteral("selection") }),
              enumeration(QStringLiteral("encoding"), { QStringLiteral("float32"), QStringLiteral("pcm24"), QStringLiteral("pcm16") }),
              enumeration(QStringLiteral("channels"), { QStringLiteral("auto"), QStringLiteral("mono"), QStringLiteral("stereo") }),
              integer(QStringLiteral("sample_rate"), false, 8000, 384000),
              boolean(QStringLiteral("overwrite")),
          } },
    };
    return specs;
}

const MethodSpec* au::mcpbridge::findMethod(const QString& name)
{
    for (const MethodSpec& spec : methodSpecs()) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

std::optional<BridgeError> au::mcpbridge::validateParams(const MethodSpec& spec, const QJsonObject& params)
{
    for (auto it = params.begin(); it != params.end(); ++it) {
        bool known = false;
        for (const ParamSpec& p : spec.params) {
            if (p.name == it.key()) {
                known = true;
                break;
            }
        }
        if (!known) {
            return invalid(QStringLiteral("unknown parameter '%1' for '%2'").arg(it.key(), spec.name));
        }
    }

    for (const ParamSpec& p : spec.params) {
        const QJsonValue value = params.value(p.name);
        if (value.isUndefined()) {
            if (p.required) {
                return invalid(QStringLiteral("missing required parameter '%1'").arg(p.name));
            }
            continue;
        }
        if (auto err = checkValue(p, value)) {
            return err;
        }
    }

    return std::nullopt;
}

std::optional<BridgeError> au::mcpbridge::parseRequest(const QJsonObject& request, QString& method, QJsonObject& params)
{
    for (auto it = request.begin(); it != request.end(); ++it) {
        if (it.key() != QLatin1String("id") && it.key() != QLatin1String("method") && it.key() != QLatin1String("params")) {
            return BridgeError { errors::InvalidRequest, QStringLiteral("unknown request field '%1'").arg(it.key()) };
        }
    }

    const QJsonValue id = request.value(QLatin1String("id"));
    if (!(id.isDouble() || (id.isString() && id.toString().size() <= 128))) {
        return BridgeError { errors::InvalidRequest, QStringLiteral("'id' must be a number or a short string") };
    }

    const QJsonValue m = request.value(QLatin1String("method"));
    if (!m.isString() || m.toString().isEmpty() || m.toString().size() > 64) {
        return BridgeError { errors::InvalidRequest, QStringLiteral("'method' must be a non-empty string") };
    }

    const QJsonValue p = request.value(QLatin1String("params"));
    if (!p.isUndefined() && !p.isNull() && !p.isObject()) {
        return BridgeError { errors::InvalidRequest, QStringLiteral("'params' must be an object") };
    }

    method = m.toString();
    params = p.toObject();
    return std::nullopt;
}

std::optional<BridgeError> au::mcpbridge::validateWavPath(const QString& path, const QString& paramName)
{
    if (!QDir::isAbsolutePath(path)) {
        return invalid(QStringLiteral("'%1' must be an absolute path").arg(paramName));
    }
    if (!path.endsWith(QLatin1String(".wav"), Qt::CaseInsensitive)) {
        return invalid(QStringLiteral("'%1' must name a .wav file").arg(paramName));
    }
    if (path.contains(QChar(0))) {
        return invalid(QStringLiteral("'%1' must not contain NUL characters").arg(paramName));
    }
    return std::nullopt;
}

bool au::mcpbridge::tokensEqual(const QByteArray& a, const QByteArray& b)
{
    // Length is not secret (fixed-size token); compare every byte regardless of mismatches.
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    }
    return diff == 0;
}
