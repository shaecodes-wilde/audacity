/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <optional>
#include <vector>

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

//! Wire protocol of the local MCP bridge.
//!
//! The bridge speaks newline-delimited JSON over an owner-only local socket.
//! Every request is {"id": <number|string>, "method": <string>, "params": <object>}.
//! The first request on a connection must be "auth" with the token that Audacity
//! wrote to an owner-only file at startup; any other first request closes the
//! connection. Only the methods listed in methodSpecs() exist: there is no way to
//! run arbitrary Audacity actions or commands through the bridge.
//!
//! This file depends on QtCore only, so it can be unit-tested without the rest of
//! Audacity.
namespace au::mcpbridge {
constexpr int PROTOCOL_VERSION = 1;

//! Upper bound for one request line, including the newline.
constexpr int MAX_REQUEST_BYTES = 64 * 1024;

namespace errors {
inline const QString InvalidRequest = QStringLiteral("invalid_request");
inline const QString Unauthenticated = QStringLiteral("unauthenticated");
inline const QString UnknownMethod = QStringLiteral("unknown_method");
inline const QString InvalidParams = QStringLiteral("invalid_params");
inline const QString NoProject = QStringLiteral("no_project");
inline const QString NotFound = QStringLiteral("not_found");
inline const QString Failed = QStringLiteral("failed");
}

struct ParamSpec {
    enum class Type {
        Number,       //! finite double within [min, max]
        Integer,      //! integral double within [min, max]
        String,       //! non-empty string up to maxLength, no NUL
        Bool,
        IntegerArray, //! array (up to maxLength entries) of integers within [min, max]
        Enum          //! one of enumValues
    };

    QString name;
    Type type = Type::String;
    bool required = false;
    double min = 0.0;
    double max = 0.0;
    int maxLength = 0;
    QStringList enumValues;
};

struct MethodSpec {
    QString name;
    std::vector<ParamSpec> params;
};

struct BridgeError {
    QString code;
    QString message;
};

//! Every method the bridge can dispatch (besides "auth").
const std::vector<MethodSpec>& methodSpecs();
const MethodSpec* findMethod(const QString& name);

//! Rejects unknown parameters, missing required ones, wrong types and
//! out-of-range values. Returns std::nullopt when params are acceptable.
std::optional<BridgeError> validateParams(const MethodSpec& spec, const QJsonObject& params);

//! Checks the id/method/params envelope. On success fills method and params.
std::optional<BridgeError> parseRequest(const QJsonObject& request, QString& method, QJsonObject& params);

//! Absolute path checks shared by import and export. Lexical only.
std::optional<BridgeError> validateWavPath(const QString& path, const QString& paramName);

//! Constant-time comparison for the auth token.
bool tokensEqual(const QByteArray& a, const QByteArray& b);
}
