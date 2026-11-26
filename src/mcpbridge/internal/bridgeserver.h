/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <map>
#include <memory>

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>

#include "bridgeprotocol.h"

class QLocalServer;
class QLocalSocket;
class QLockFile;
class QTimer;

namespace au::mcpbridge {
struct BridgeResult {
    bool ok = true;
    QJsonValue value;
    BridgeError error;

    static BridgeResult success(const QJsonValue& v) { return { true, v, {} }; }
    static BridgeResult failure(const QString& code, const QString& message) { return { false, {}, { code, message } }; }
};

//! Executes already-validated requests. Called on the thread that owns the
//! BridgeServer (the GUI thread in Audacity), one request at a time.
class IBridgeHandler
{
public:
    virtual ~IBridgeHandler() = default;
    virtual BridgeResult handle(const QString& method, const QJsonObject& params) = 0;
};

//! Owner-only local socket with token authentication.
//!
//! start() creates (or reuses) a private directory, writes a fresh random token
//! to "<dir>/token" with mode 0600 and listens on "<dir>/bridge.sock" with
//! QLocalServer::UserAccessOption. Clients must authenticate with that token as
//! their first request. Requests are processed strictly one at a time; requests
//! that arrive while a handler runs (for example, while a nested event loop is
//! spinning for a progress dialog) wait until it returns.
class BridgeServer : public QObject
{
    Q_OBJECT

public:
    static constexpr int MAX_CLIENTS = 4;
    static constexpr int AUTH_TIMEOUT_MS = 10000;

    explicit BridgeServer(IBridgeHandler* handler, QObject* parent = nullptr);
    ~BridgeServer() override;

    //! Default directory: $AUDACITY_MCP_DIR, else <runtime dir>/audacity-mcp.
    static QString defaultDirectory();

    bool start(const QString& directory, QString* errorMessage = nullptr);
    void stop();
    bool isListening() const;

    QString socketPath() const;
    QString tokenPath() const;

private:
    struct Client {
        QPointer<QLocalSocket> socket;
        QByteArray buffer;
        bool authenticated = false;
        QTimer* authTimer = nullptr;
    };

    void onNewConnection();
    void onReadyRead(QLocalSocket* socket);
    void onDisconnected(QLocalSocket* socket);
    void processPending();
    bool processLine(QLocalSocket* socket, const QByteArray& line); //! returns false when the client must be dropped
    void sendResult(QLocalSocket* socket, const QJsonValue& id, const QJsonValue& result);
    void sendError(QLocalSocket* socket, const QJsonValue& id, const QString& code, const QString& message);
    void dropClient(QLocalSocket* socket);

    IBridgeHandler* m_handler = nullptr;
    QLocalServer* m_server = nullptr;
    std::unique_ptr<QLockFile> m_lock;
    QString m_directory;
    QByteArray m_token;
    std::map<QLocalSocket*, Client> m_clients;
    bool m_dispatching = false;
};
}
