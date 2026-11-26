/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity: A Digital Audio Editor
 */
#include "bridgeserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace au::mcpbridge;

namespace {
const QString SOCKET_NAME = QStringLiteral("bridge.sock");
const QString TOKEN_NAME = QStringLiteral("token");

QByteArray generateToken()
{
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator* rng = QRandomGenerator::system();
    for (qsizetype i = 0; i < raw.size(); i += 4) {
        const quint32 word = rng->generate();
        for (int b = 0; b < 4 && i + b < raw.size(); ++b) {
            raw[i + b] = static_cast<char>((word >> (8 * b)) & 0xff);
        }
    }
    return raw.toHex();
}

bool setError(QString* out, const QString& message)
{
    if (out) {
        *out = message;
    }
    return false;
}

//! Creates the directory if needed and makes sure only the current user can use it.
bool preparePrivateDirectory(const QString& dir, QString* error)
{
#ifdef Q_OS_UNIX
    const QByteArray native = QFile::encodeName(dir);
    if (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST) {
        return setError(error, QStringLiteral("cannot create %1: %2").arg(dir, QString::fromLocal8Bit(std::strerror(errno))));
    }

    struct stat st {};
    if (::lstat(native.constData(), &st) != 0) {
        return setError(error, QStringLiteral("cannot stat %1").arg(dir));
    }
    if (!S_ISDIR(st.st_mode)) {
        return setError(error, QStringLiteral("%1 is not a directory (symlinks are refused)").arg(dir));
    }
    if (st.st_uid != ::geteuid()) {
        return setError(error, QStringLiteral("%1 is owned by another user").arg(dir));
    }
    if ((st.st_mode & 077) != 0) {
        return setError(error, QStringLiteral("%1 must not be accessible by group or others (chmod 700)").arg(dir));
    }
    return true;
#else
    if (!QDir().mkpath(dir)) {
        return setError(error, QStringLiteral("cannot create %1").arg(dir));
    }
    return true;
#endif
}

bool writeTokenFile(const QString& path, const QByteArray& token, QString* error)
{
#ifdef Q_OS_UNIX
    const QByteArray native = QFile::encodeName(path);
    ::unlink(native.constData());
    const int fd = ::open(native.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return setError(error, QStringLiteral("cannot create token file %1").arg(path));
    }
    const QByteArray content = token + '\n';
    const ssize_t written = ::write(fd, content.constData(), static_cast<size_t>(content.size()));
    ::close(fd);
    if (written != content.size()) {
        ::unlink(native.constData());
        return setError(error, QStringLiteral("cannot write token file %1").arg(path));
    }
    return true;
#else
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return setError(error, QStringLiteral("cannot create token file %1").arg(path));
    }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(token + '\n');
    return true;
#endif
}

bool anotherInstanceListening(const QString& socketPath)
{
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    const bool connected = probe.waitForConnected(500);
    probe.abort();
    return connected;
}
}

BridgeServer::BridgeServer(IBridgeHandler* handler, QObject* parent)
    : QObject(parent), m_handler(handler)
{
}

BridgeServer::~BridgeServer()
{
    stop();
}

QString BridgeServer::defaultDirectory()
{
    const QString fromEnv = qEnvironmentVariable("AUDACITY_MCP_DIR");
    if (!fromEnv.isEmpty()) {
        return QDir::cleanPath(fromEnv);
    }

    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    return QDir::cleanPath(base + QStringLiteral("/audacity-mcp"));
}

QString BridgeServer::socketPath() const
{
    return m_directory + QLatin1Char('/') + SOCKET_NAME;
}

QString BridgeServer::tokenPath() const
{
    return m_directory + QLatin1Char('/') + TOKEN_NAME;
}

bool BridgeServer::isListening() const
{
    return m_server && m_server->isListening();
}

bool BridgeServer::start(const QString& directory, QString* errorMessage)
{
    stop();

    if (!QDir::isAbsolutePath(directory)) {
        return setError(errorMessage, QStringLiteral("bridge directory must be absolute: %1").arg(directory));
    }
    m_directory = QDir::cleanPath(directory);

    if (!preparePrivateDirectory(m_directory, errorMessage)) {
        return false;
    }

    // Serialize startup and cleanup before either process can touch the shared token.
    auto lock = std::make_unique<QLockFile>(m_directory + QStringLiteral("/bridge.lock"));
    lock->setStaleLockTime(0);
    if (!lock->tryLock()) {
        return setError(errorMessage, QStringLiteral("cannot acquire the MCP bridge lock in %1").arg(m_directory));
    }

    const QString sockPath = socketPath();
    if (QFile::exists(sockPath)) {
        if (anotherInstanceListening(sockPath)) {
            return setError(errorMessage, QStringLiteral("another Audacity MCP bridge is already listening on %1").arg(sockPath));
        }
        QLocalServer::removeServer(sockPath);
    }

    m_token = generateToken();
    if (!writeTokenFile(tokenPath(), m_token, errorMessage)) {
        m_token.clear();
        return false;
    }

    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    m_server->setMaxPendingConnections(MAX_CLIENTS);
    if (!m_server->listen(sockPath)) {
        const QString err = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        QFile::remove(tokenPath());
        m_token.clear();
        return setError(errorMessage, QStringLiteral("cannot listen on %1: %2").arg(sockPath, err));
    }

    connect(m_server, &QLocalServer::newConnection, this, &BridgeServer::onNewConnection);
    m_lock = std::move(lock);
    return true;
}

void BridgeServer::stop()
{
    const auto clients = m_clients;
    for (const auto& entry : clients) {
        if (entry.second.socket) {
            entry.second.socket->abort();
        }
    }
    m_clients.clear();

    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
        QLocalServer::removeServer(socketPath());
    }

    if (!m_token.isEmpty()) {
        QFile::remove(tokenPath());
        m_token.fill('\0');
        m_token.clear();
    }
    m_lock.reset();
}

void BridgeServer::onNewConnection()
{
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        if (static_cast<int>(m_clients.size()) >= MAX_CLIENTS) {
            sendError(socket, QJsonValue(), errors::Failed, QStringLiteral("too many clients"));
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }

        Client client;
        client.socket = socket;
        client.authTimer = new QTimer(socket);
        client.authTimer->setSingleShot(true);
        connect(client.authTimer, &QTimer::timeout, socket, [this, socket]() {
            sendError(socket, QJsonValue(), errors::Unauthenticated, QStringLiteral("authentication timed out"));
            dropClient(socket);
        });
        client.authTimer->start(AUTH_TIMEOUT_MS);

        m_clients.emplace(socket, client);

        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() { onDisconnected(socket); });

        if (socket->bytesAvailable() > 0) {
            onReadyRead(socket);
        }
    }
}

void BridgeServer::onReadyRead(QLocalSocket* socket)
{
    auto it = m_clients.find(socket);
    if (it == m_clients.end()) {
        return;
    }

    it->second.buffer.append(socket->readAll());
    const qsizetype newline = it->second.buffer.indexOf('\n');
    if ((newline < 0 && it->second.buffer.size() > MAX_REQUEST_BYTES) || newline > MAX_REQUEST_BYTES) {
        sendError(socket, QJsonValue(), errors::InvalidRequest, QStringLiteral("request too large"));
        dropClient(socket);
        return;
    }

    processPending();
}

void BridgeServer::onDisconnected(QLocalSocket* socket)
{
    auto it = m_clients.find(socket);
    if (it == m_clients.end()) {
        return;
    }
    m_clients.erase(it);
    socket->deleteLater();
}

void BridgeServer::dropClient(QLocalSocket* socket)
{
    m_clients.erase(socket);
    if (socket) {
        socket->disconnect(this);
        socket->flush();
        socket->disconnectFromServer();
        socket->deleteLater();
    }
}

void BridgeServer::processPending()
{
    //! A handler may spin a nested event loop (progress dialogs do). Never run a
    //! second request inside it; the outer call picks the rest up afterwards.
    if (m_dispatching) {
        return;
    }

    bool progressed = true;
    while (progressed) {
        progressed = false;

        std::vector<QLocalSocket*> sockets;
        sockets.reserve(m_clients.size());
        for (const auto& entry : m_clients) {
            sockets.push_back(entry.first);
        }

        for (QLocalSocket* socket : sockets) {
            auto it = m_clients.find(socket);
            if (it == m_clients.end()) {
                continue;
            }
            const qsizetype newline = it->second.buffer.indexOf('\n');
            if (newline < 0) {
                continue;
            }

            const QByteArray line = it->second.buffer.left(newline);
            it->second.buffer.remove(0, newline + 1);

            m_dispatching = true;
            const bool keep = processLine(socket, line);
            m_dispatching = false;
            progressed = true;

            if (!keep && m_clients.count(socket)) {
                dropClient(socket);
            }
        }
    }
}

bool BridgeServer::processLine(QLocalSocket* socket, const QByteArray& line)
{
    //! Look the client up again on every use: a nested event loop inside the
    //! handler may disconnect and erase it.
    auto client = [this, socket]() -> Client* {
        auto it = m_clients.find(socket);
        return it == m_clients.end() || !it->second.socket ? nullptr : &it->second;
    };
    if (!client()) {
        return false;
    }
    const bool authenticated = client()->authenticated;

    if (line.trimmed().isEmpty()) {
        return true;
    }

    QJsonParseError parseError {};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        sendError(socket, QJsonValue(), errors::InvalidRequest, QStringLiteral("request must be a JSON object on one line"));
        return authenticated;
    }

    const QJsonObject request = doc.object();
    const QJsonValue id = request.value(QLatin1String("id"));
    QString method;
    QJsonObject params;
    if (auto err = parseRequest(request, method, params)) {
        sendError(socket, id, err->code, err->message);
        return authenticated;
    }

    if (!authenticated) {
        const QJsonValue token = params.value(QLatin1String("token"));
        const bool ok = method == QLatin1String("auth")
                        && params.size() == 1
                        && token.isString()
                        && !m_token.isEmpty()
                        && tokensEqual(token.toString().toUtf8(), m_token);
        if (!ok) {
            sendError(socket, id, errors::Unauthenticated, QStringLiteral("the first request must be 'auth' with the current token"));
            return false;
        }
        client()->authenticated = true;
        if (client()->authTimer) {
            client()->authTimer->stop();
        }
        QJsonObject result;
        result.insert(QStringLiteral("protocol"), PROTOCOL_VERSION);
        result.insert(QStringLiteral("application"), QCoreApplication::applicationName());
        result.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
        sendResult(socket, id, result);
        return true;
    }

    if (method == QLatin1String("auth")) {
        sendError(socket, id, errors::InvalidRequest, QStringLiteral("already authenticated"));
        return true;
    }

    const MethodSpec* spec = findMethod(method);
    if (!spec) {
        sendError(socket, id, errors::UnknownMethod, QStringLiteral("unknown method '%1'").arg(method));
        return true;
    }

    if (auto err = validateParams(*spec, params)) {
        sendError(socket, id, err->code, err->message);
        return true;
    }

    const BridgeResult result = m_handler
                                ? m_handler->handle(method, params)
                                : BridgeResult::failure(errors::Failed, QStringLiteral("no handler"));

    //! The socket can go away while the handler runs (nested event loops).
    if (!client()) {
        return false;
    }

    if (result.ok) {
        sendResult(socket, id, result.value);
    } else {
        sendError(socket, id, result.error.code, result.error.message);
    }
    return true;
}

void BridgeServer::sendResult(QLocalSocket* socket, const QJsonValue& id, const QJsonValue& result)
{
    if (!socket) {
        return;
    }
    QJsonObject response;
    response.insert(QStringLiteral("id"), id.isUndefined() ? QJsonValue() : id);
    response.insert(QStringLiteral("result"), result);
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
    socket->flush();
}

void BridgeServer::sendError(QLocalSocket* socket, const QJsonValue& id, const QString& code, const QString& message)
{
    if (!socket) {
        return;
    }
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    QJsonObject response;
    response.insert(QStringLiteral("id"), id.isUndefined() ? QJsonValue() : id);
    response.insert(QStringLiteral("error"), error);
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
    socket->flush();
}
