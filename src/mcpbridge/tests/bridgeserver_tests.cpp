/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Audacity: A Digital Audio Editor
 */
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include "mcpbridge/internal/bridgeprotocol.h"
#include "mcpbridge/internal/bridgeserver.h"

using namespace au::mcpbridge;

namespace {
class RecordingHandler : public IBridgeHandler
{
public:
    BridgeResult handle(const QString& method, const QJsonObject& params) override
    {
        calls.push_back(method);
        lastParams = params;
        if (method == QLatin1String("undo")) {
            return BridgeResult::failure(errors::Failed, QStringLiteral("nothing to undo"));
        }
        QJsonObject r;
        r.insert(QStringLiteral("method"), method);
        return BridgeResult::success(r);
    }

    std::vector<QString> calls;
    QJsonObject lastParams;
};

QJsonObject obj(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

std::optional<BridgeError> validate(const char* method, const char* paramsJson)
{
    const MethodSpec* spec = findMethod(QString::fromLatin1(method));
    EXPECT_NE(spec, nullptr) << method;
    if (!spec) {
        return BridgeError { errors::UnknownMethod, {} };
    }
    return validateParams(*spec, obj(paramsJson));
}

class Client
{
public:
    explicit Client(const QString& path)
    {
        m_socket.connectToServer(path);
        waitFor([this]() { return m_socket.state() == QLocalSocket::ConnectedState; });
    }

    bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }

    void sendRaw(const QByteArray& data)
    {
        m_socket.write(data);
        m_socket.flush();
    }

    QJsonObject call(const QJsonObject& request)
    {
        sendRaw(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        return readResponse();
    }

    QJsonObject call(int id, const QString& method, const QJsonObject& params = {})
    {
        QJsonObject req;
        req.insert(QStringLiteral("id"), id);
        req.insert(QStringLiteral("method"), method);
        req.insert(QStringLiteral("params"), params);
        return call(req);
    }

    QJsonObject readResponse()
    {
        waitFor([this]() { return m_buffer.contains('\n') || m_socket.state() != QLocalSocket::ConnectedState; });
        const qsizetype nl = m_buffer.indexOf('\n');
        if (nl < 0) {
            return {};
        }
        const QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        return QJsonDocument::fromJson(line).object();
    }

    bool waitForDisconnect()
    {
        waitFor([this]() { return m_socket.state() == QLocalSocket::UnconnectedState; });
        return m_socket.state() == QLocalSocket::UnconnectedState;
    }

private:
    template<typename Pred>
    void waitFor(Pred pred, int timeoutMs = 3000)
    {
        QDeadlineTimer deadline(timeoutMs);
        while (!pred() && !deadline.hasExpired()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            m_socket.waitForReadyRead(5);
            m_buffer.append(m_socket.readAll());
        }
        m_buffer.append(m_socket.readAll());
    }

    QLocalSocket m_socket;
    QByteArray m_buffer;
};

QByteArray readToken(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll().trimmed();
}

QJsonObject authParams(const QByteArray& token)
{
    QJsonObject p;
    p.insert(QStringLiteral("token"), QString::fromLatin1(token));
    return p;
}
}

// ---------------------------------------------------------------------------
// Protocol validation
// ---------------------------------------------------------------------------

TEST(McpBridgeProtocol, OnlyAllowlistedMethodsExist)
{
    EXPECT_NE(findMethod(QStringLiteral("export_wav")), nullptr);
    EXPECT_EQ(findMethod(QStringLiteral("auth")), nullptr); //! handled by the server, never dispatched
    EXPECT_EQ(findMethod(QStringLiteral("run_action")), nullptr);
    EXPECT_EQ(findMethod(QStringLiteral("action://file-save")), nullptr);
    EXPECT_EQ(findMethod(QString()), nullptr);
}

TEST(McpBridgeProtocol, RejectsUnknownAndMissingParams)
{
    EXPECT_FALSE(validate("ping", "{}"));
    EXPECT_TRUE(validate("ping", R"({"x":1})"));
    EXPECT_TRUE(validate("import_wav", "{}"));
    EXPECT_TRUE(validate("set_clip_gain", R"({"track_id":1,"clip_id":2})"));
    EXPECT_FALSE(validate("set_clip_gain", R"({"track_id":1,"clip_id":2,"gain_db":-3.5})"));
}

TEST(McpBridgeProtocol, ChecksTypesAndRanges)
{
    EXPECT_TRUE(validate("set_clip_gain", R"({"track_id":1.5,"clip_id":2,"gain_db":0})"));
    EXPECT_TRUE(validate("set_clip_gain", R"({"track_id":-1,"clip_id":2,"gain_db":0})"));
    EXPECT_TRUE(validate("set_clip_gain", R"({"track_id":"1","clip_id":2,"gain_db":0})"));
    EXPECT_TRUE(validate("set_clip_gain", R"({"track_id":1,"clip_id":2,"gain_db":12})"));
    EXPECT_TRUE(validate("set_clip_gain", R"({"track_id":1,"clip_id":2,"gain_db":-61})"));
    EXPECT_TRUE(validate("set_selection", R"({"start":-1,"end":2})"));
    EXPECT_TRUE(validate("set_selection", R"({"start":0,"end":2,"track_ids":[1,"2"]})"));
    EXPECT_TRUE(validate("set_selection", R"({"start":0,"end":2,"track_ids":[1,-2]})"));
    EXPECT_TRUE(validate("set_selection", R"({"start":0,"end":2,"track_ids":1})"));
    EXPECT_FALSE(validate("set_selection", R"({"start":0,"end":2,"track_ids":[1,2]})"));
    EXPECT_TRUE(validate("list_tracks", R"({"include_items":"yes"})"));
    EXPECT_FALSE(validate("list_tracks", R"({"include_items":false})"));
    EXPECT_TRUE(validate("add_label", R"({"start":1,"text":""})"));
    EXPECT_FALSE(validate("add_label", R"({"start":1,"end":2,"text":"Intro"})"));
}

TEST(McpBridgeProtocol, ExportEnumsAndRates)
{
    EXPECT_FALSE(validate("export_wav", R"({"path":"/tmp/a.wav","scope":"selection","encoding":"pcm24","channels":"stereo","sample_rate":48000})"));
    EXPECT_TRUE(validate("export_wav", R"({"path":"/tmp/a.wav","scope":"loop"})"));
    EXPECT_TRUE(validate("export_wav", R"({"path":"/tmp/a.wav","encoding":"mp3"})"));
    EXPECT_TRUE(validate("export_wav", R"({"path":"/tmp/a.wav","channels":2})"));
    EXPECT_TRUE(validate("export_wav", R"({"path":"/tmp/a.wav","sample_rate":44})"));
    EXPECT_TRUE(validate("export_wav", R"({"path":"/tmp/a.wav","sample_rate":48000.5})"));
}

TEST(McpBridgeProtocol, WavPaths)
{
    EXPECT_FALSE(validateWavPath(QStringLiteral("/home/u/out.WAV"), QStringLiteral("path")));
    EXPECT_TRUE(validateWavPath(QStringLiteral("relative/out.wav"), QStringLiteral("path")));
    EXPECT_TRUE(validateWavPath(QStringLiteral("/home/u/out.mp3"), QStringLiteral("path")));
    EXPECT_TRUE(validateWavPath(QStringLiteral("/home/u/out"), QStringLiteral("path")));
}

TEST(McpBridgeProtocol, RequestEnvelope)
{
    QString method;
    QJsonObject params;
    EXPECT_FALSE(parseRequest(obj(R"({"id":1,"method":"ping"})"), method, params));
    EXPECT_EQ(method, QStringLiteral("ping"));
    EXPECT_FALSE(parseRequest(obj(R"({"id":"a","method":"ping","params":{}})"), method, params));
    EXPECT_TRUE(parseRequest(obj(R"({"method":"ping"})"), method, params));
    EXPECT_TRUE(parseRequest(obj(R"({"id":1,"method":""})"), method, params));
    EXPECT_TRUE(parseRequest(obj(R"({"id":1,"method":"ping","params":[1]})"), method, params));
    EXPECT_TRUE(parseRequest(obj(R"({"id":1,"method":"ping","extra":true})"), method, params));
}

TEST(McpBridgeProtocol, TokenComparison)
{
    EXPECT_TRUE(tokensEqual("abcd", "abcd"));
    EXPECT_FALSE(tokensEqual("abcd", "abce"));
    EXPECT_FALSE(tokensEqual("abcd", "abc"));
    EXPECT_FALSE(tokensEqual("", "a"));
}

// ---------------------------------------------------------------------------
// Server: authentication, framing, dispatch
// ---------------------------------------------------------------------------

class McpBridgeServerTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_tmp.isValid());
        m_dir = m_tmp.path() + QStringLiteral("/bridge");
        m_server = std::make_unique<BridgeServer>(&m_handler);
        QString error;
        ASSERT_TRUE(m_server->start(m_dir, &error)) << error.toStdString();
        m_token = readToken(m_server->tokenPath());
        ASSERT_EQ(m_token.size(), 64);
    }

    void TearDown() override
    {
        m_server.reset();
    }

    QTemporaryDir m_tmp;
    QString m_dir;
    RecordingHandler m_handler;
    std::unique_ptr<BridgeServer> m_server;
    QByteArray m_token;
};

TEST_F(McpBridgeServerTests, FilesAreOwnerOnly)
{
#ifdef Q_OS_UNIX
    struct stat st {};
    ASSERT_EQ(::stat(QFile::encodeName(m_dir).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0700u);
    ASSERT_EQ(::stat(QFile::encodeName(m_server->tokenPath()).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
    ASSERT_EQ(::stat(QFile::encodeName(m_server->socketPath()).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 077, 0u);
#endif
}

TEST_F(McpBridgeServerTests, RequestBeforeAuthIsRejectedAndDisconnected)
{
    Client c(m_server->socketPath());
    ASSERT_TRUE(c.connected());
    const QJsonObject r = c.call(1, QStringLiteral("get_project"));
    EXPECT_EQ(r.value("error").toObject().value("code").toString(), errors::Unauthenticated);
    EXPECT_TRUE(c.waitForDisconnect());
    EXPECT_TRUE(m_handler.calls.empty());
}

TEST_F(McpBridgeServerTests, WrongTokenIsRejected)
{
    Client c(m_server->socketPath());
    QByteArray wrong = m_token;
    wrong[0] = wrong[0] == 'a' ? 'b' : 'a';
    const QJsonObject r = c.call(1, QStringLiteral("auth"), authParams(wrong));
    EXPECT_EQ(r.value("error").toObject().value("code").toString(), errors::Unauthenticated);
    EXPECT_TRUE(c.waitForDisconnect());
}

TEST_F(McpBridgeServerTests, AuthWithExtraParamsIsRejected)
{
    Client c(m_server->socketPath());
    QJsonObject p = authParams(m_token);
    p.insert(QStringLiteral("method"), QStringLiteral("export_wav"));
    const QJsonObject r = c.call(1, QStringLiteral("auth"), p);
    EXPECT_EQ(r.value("error").toObject().value("code").toString(), errors::Unauthenticated);
    EXPECT_TRUE(c.waitForDisconnect());
}

TEST_F(McpBridgeServerTests, AuthenticatedDispatch)
{
    Client c(m_server->socketPath());
    const QJsonObject auth = c.call(1, QStringLiteral("auth"), authParams(m_token));
    EXPECT_EQ(auth.value("id").toInt(), 1);
    EXPECT_EQ(auth.value("result").toObject().value("protocol").toInt(), PROTOCOL_VERSION);

    const QJsonObject pong = c.call(2, QStringLiteral("ping"));
    EXPECT_EQ(pong.value("id").toInt(), 2);
    EXPECT_EQ(pong.value("result").toObject().value("method").toString(), QStringLiteral("ping"));

    const QJsonObject failed = c.call(3, QStringLiteral("undo"));
    EXPECT_EQ(failed.value("error").toObject().value("code").toString(), errors::Failed);

    ASSERT_EQ(m_handler.calls.size(), 2u);
    EXPECT_TRUE(c.connected());
}

TEST_F(McpBridgeServerTests, InvalidRequestsNeverReachHandler)
{
    Client c(m_server->socketPath());
    c.call(1, QStringLiteral("auth"), authParams(m_token));

    EXPECT_EQ(c.call(2, QStringLiteral("run_action")).value("error").toObject().value("code").toString(), errors::UnknownMethod);

    QJsonObject badParams;
    badParams.insert(QStringLiteral("path"), QStringLiteral("/tmp/x.wav"));
    badParams.insert(QStringLiteral("command"), QStringLiteral("rm -rf"));
    EXPECT_EQ(c.call(3, QStringLiteral("import_wav"), badParams).value("error").toObject().value("code").toString(),
              errors::InvalidParams);

    c.sendRaw("this is not json\n");
    EXPECT_EQ(c.readResponse().value("error").toObject().value("code").toString(), errors::InvalidRequest);

    EXPECT_EQ(c.call(4, QStringLiteral("auth"), authParams(m_token)).value("error").toObject().value("code").toString(),
              errors::InvalidRequest);

    EXPECT_TRUE(m_handler.calls.empty());
    EXPECT_TRUE(c.connected());
}

TEST_F(McpBridgeServerTests, SeveralRequestsInOneWrite)
{
    Client c(m_server->socketPath());
    const QByteArray batch = QByteArray(R"({"id":1,"method":"auth","params":{"token":")") + m_token + R"("}})" "\n"
                             R"({"id":2,"method":"ping"})" "\n"
                             R"({"id":3,"method":"get_project"})" "\n";
    c.sendRaw(batch);
    EXPECT_EQ(c.readResponse().value("id").toInt(), 1);
    EXPECT_EQ(c.readResponse().value("id").toInt(), 2);
    EXPECT_EQ(c.readResponse().value("id").toInt(), 3);
    EXPECT_EQ(m_handler.calls.size(), 2u);
}

TEST_F(McpBridgeServerTests, OversizedRequestDropsClient)
{
    Client c(m_server->socketPath());
    c.call(1, QStringLiteral("auth"), authParams(m_token));
    c.sendRaw(QByteArray(MAX_REQUEST_BYTES + 10, 'x'));
    EXPECT_EQ(c.readResponse().value("error").toObject().value("code").toString(), errors::InvalidRequest);
    EXPECT_TRUE(c.waitForDisconnect());
}

TEST_F(McpBridgeServerTests, SecondServerOnSameDirectoryIsRefused)
{
    RecordingHandler other;
    BridgeServer second(&other);
    QString error;
    EXPECT_FALSE(second.start(m_dir, &error));
    EXPECT_FALSE(error.isEmpty());
    //! The first server's token must survive the failed attempt.
    EXPECT_EQ(readToken(m_server->tokenPath()), m_token);
    Client client(m_server->socketPath());
    EXPECT_FALSE(client.call(1, QStringLiteral("auth"), authParams(m_token)).contains("error"));
    EXPECT_FALSE(client.call(2, QStringLiteral("ping")).contains("error"));
}

TEST_F(McpBridgeServerTests, StopRemovesTokenAndSocket)
{
    const QString token = m_server->tokenPath();
    const QString sock = m_server->socketPath();
    m_server->stop();
    EXPECT_FALSE(QFileInfo::exists(token));
    EXPECT_FALSE(QFileInfo::exists(sock));
    EXPECT_FALSE(QFileInfo::exists(m_dir + QStringLiteral("/bridge.lock")));
}

TEST_F(McpBridgeServerTests, NewTokenOnEveryStart)
{
    m_server->stop();
    QString error;
    ASSERT_TRUE(m_server->start(m_dir, &error)) << error.toStdString();
    const QByteArray fresh = readToken(m_server->tokenPath());
    EXPECT_EQ(fresh.size(), 64);
    EXPECT_NE(fresh, m_token);
}

#ifdef Q_OS_UNIX
TEST(McpBridgeServerDirectory, RefusesSharedDirectory)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path() + QStringLiteral("/shared");
    ASSERT_EQ(::mkdir(QFile::encodeName(dir).constData(), 0700), 0);
    ASSERT_EQ(::chmod(QFile::encodeName(dir).constData(), 0755), 0);

    RecordingHandler h;
    BridgeServer server(&h);
    QString error;
    EXPECT_FALSE(server.start(dir, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("group or others"))) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(dir + QStringLiteral("/token")));
}

TEST(McpBridgeServerDirectory, RefusesSymlinkedDirectory)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString real = tmp.path() + QStringLiteral("/real");
    const QString link = tmp.path() + QStringLiteral("/link");
    ASSERT_EQ(::mkdir(QFile::encodeName(real).constData(), 0700), 0);
    ASSERT_TRUE(QFile::link(real, link));

    RecordingHandler h;
    BridgeServer server(&h);
    QString error;
    EXPECT_FALSE(server.start(link, &error));
}

TEST(McpBridgeServerDirectory, RefusesRelativeDirectory)
{
    RecordingHandler h;
    BridgeServer server(&h);
    QString error;
    EXPECT_FALSE(server.start(QStringLiteral("relative/dir"), &error));
}
#endif
