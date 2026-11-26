#include "providers/twitch/WatchHeartbeatService.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <boost/beast/zlib/deflate_stream.hpp>
#include <boost/crc.hpp>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <cerrno>

#ifdef Q_OS_WIN
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#elif defined(Q_OS_UNIX)
#    include <csignal>
#    include <sys/types.h>
#endif

namespace chatterino {

namespace {

constexpr const char *SPADE_URL = "https://spade.twitch.tv/track";
constexpr int HEARTBEAT_INTERVAL_MS = 60 * 1000;
constexpr int PROCESS_WATCH_INTERVAL_MS = 5 * 1000;

bool isProcessAlive(qint64 pid)
{
    if (pid <= 0)
    {
        return false;
    }

#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(pid));
    if (handle == nullptr)
    {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const DWORD waitResult = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return waitResult == WAIT_TIMEOUT;
#elif defined(Q_OS_UNIX)
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
    {
        return true;
    }
    return errno == EPERM;
#else
    return false;
#endif
}

bool isValidTwitchLogin(const QString &login)
{
    if (login.isEmpty() || login.size() > 32)
    {
        return false;
    }
    for (const QChar ch : login)
    {
        const ushort c = ch.unicode();
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

}  // namespace

namespace watchheartbeat::detail {

QByteArray buildLegacySpadePayload(const WatchTarget &target,
                                   const QString &userId)
{
    QJsonObject props;
    props.insert("broadcast_id", target.broadcastId);
    props.insert("channel_id", target.channelId);
    props.insert("channel", target.login);
    props.insert("hidden", false);
    props.insert("live", true);
    props.insert("location", QStringLiteral("channel"));
    props.insert("logged_in", true);
    props.insert("muted", false);
    props.insert("player", QStringLiteral("site"));
    props.insert("user_id", userId);

    QJsonObject eventObj;
    eventObj.insert("event", QStringLiteral("minute-watched"));
    eventObj.insert("properties", props);

    QJsonArray arr;
    arr.append(eventObj);

    const auto compactJson = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    return compactJson.toBase64();
}

QByteArray buildDropsSpadeJson(const WatchTarget &target, const QString &userId,
                               const QString &clientTimeIso)
{
    QJsonObject props;
    props.insert("broadcast_id", target.broadcastId);
    props.insert("channel_id", target.channelId);
    props.insert("channel", target.login);
    props.insert("client_time", clientTimeIso);
    props.insert("game", target.gameName);
    props.insert("game_id", target.gameId);
    props.insert("hidden", false);
    props.insert("is_live", true);
    props.insert("live", true);
    props.insert("logged_in", true);
    props.insert("minutes_logged", 1);
    props.insert("muted", false);
    props.insert("user_id", userId);

    QJsonObject eventObj;
    eventObj.insert("event", QStringLiteral("minute-watched"));
    eventObj.insert("properties", props);

    QJsonArray arr;
    arr.append(eventObj);

    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QByteArray gzipCompress(const QByteArray &input)
{
    static constexpr std::array<unsigned char, 10> GZIP_HEADER = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    };

    QByteArray out;
    out.append(reinterpret_cast<const char *>(GZIP_HEADER.data()),
               qsizetype(GZIP_HEADER.size()));

    boost::beast::zlib::deflate_stream stream;
    boost::beast::zlib::z_params params{};
    params.next_in = input.constData();
    params.avail_in = size_t(input.size());

    std::array<char, 4096> chunk{};
    boost::system::error_code ec;
    do
    {
        params.next_out = chunk.data();
        params.avail_out = chunk.size();
        stream.write(params, boost::beast::zlib::Flush::finish, ec);
        const auto produced = qsizetype(chunk.size() - params.avail_out);
        if (produced > 0)
        {
            out.append(chunk.data(), produced);
        }
        if (ec && ec != boost::beast::zlib::error::end_of_stream)
        {
            break;
        }
    } while (ec != boost::beast::zlib::error::end_of_stream);

    boost::crc_32_type crc;
    if (!input.isEmpty())
    {
        crc.process_bytes(input.constData(), size_t(input.size()));
    }

    std::array<char, 8> trailer{};
    qToLittleEndian<quint32>(quint32(crc.checksum()), trailer.data());
    qToLittleEndian<quint32>(quint32(input.size()), trailer.data() + 4);
    out.append(trailer.data(), qsizetype(trailer.size()));

    return out;
}

std::optional<QString> extractTwitchLoginFromUrl(const QString &urlOrTarget)
{
    auto trimmed = urlOrTarget.trimmed();
    if (trimmed.isEmpty())
    {
        return std::nullopt;
    }

    if (!trimmed.contains('/') && !trimmed.contains(':'))
    {
        const auto candidate = trimmed.toLower();
        if (isValidTwitchLogin(candidate))
        {
            return candidate;
        }
        return std::nullopt;
    }

    if (!trimmed.contains("://"))
    {
        trimmed.prepend(QStringLiteral("https://"));
    }

    const QUrl url(trimmed);
    if (!url.isValid())
    {
        return std::nullopt;
    }

    const auto host = url.host().toLower();
    if (host != QLatin1String("twitch.tv") &&
        host != QLatin1String("www.twitch.tv") &&
        host != QLatin1String("m.twitch.tv") &&
        host != QLatin1String("player.twitch.tv"))
    {
        return std::nullopt;
    }

    if (host == QLatin1String("player.twitch.tv"))
    {
        const auto channel =
            QUrlQuery(url).queryItemValue(QStringLiteral("channel")).trimmed().toLower();
        if (isValidTwitchLogin(channel))
        {
            return channel;
        }
        return std::nullopt;
    }

    const auto segments = url.path().split('/', Qt::SkipEmptyParts);
    if (segments.isEmpty())
    {
        return std::nullopt;
    }

    QString login = segments.first().trimmed().toLower();
    if (login == QLatin1String("popout") && segments.size() >= 2)
    {
        login = segments.at(1).trimmed().toLower();
    }
    if (login == QLatin1String("directory") ||
        login == QLatin1String("videos") || login == QLatin1String("clips") ||
        login == QLatin1String("settings") || login == QLatin1String("drops"))
    {
        return std::nullopt;
    }

    if (isValidTwitchLogin(login))
    {
        return login;
    }
    return std::nullopt;
}

}  // namespace watchheartbeat::detail

WatchHeartbeatService::WatchHeartbeatService(QObject *parent)
    : QObject(parent)
{
    this->heartbeatTimer_.setInterval(HEARTBEAT_INTERVAL_MS);
    this->heartbeatTimer_.setSingleShot(false);
    QObject::connect(&this->heartbeatTimer_, &QTimer::timeout, this,
                     &WatchHeartbeatService::onHeartbeatTick);

    this->processWatchTimer_.setInterval(PROCESS_WATCH_INTERVAL_MS);
    this->processWatchTimer_.setSingleShot(false);
    QObject::connect(&this->processWatchTimer_, &QTimer::timeout, this,
                     &WatchHeartbeatService::onProcessWatchTick);
}

WatchHeartbeatService::~WatchHeartbeatService()
{
    this->stop();
}

void WatchHeartbeatService::startWatching(const QString &channelLogin,
                                          const QString &sourceTag)
{
    const auto normalizedLogin = channelLogin.trimmed().toLower();
    if (normalizedLogin.isEmpty())
    {
        return;
    }

    ++this->generation_;
    if (this->trackedSourceTag_ != sourceTag)
    {
        this->trackedPid_ = 0;
        this->trackedSourceTag_.clear();
        this->processWatchTimer_.stop();
    }

    watchheartbeat::detail::WatchTarget target;
    target.login = normalizedLogin;
    target.sourceTag = sourceTag;
    target.startedAt = QDateTime::currentDateTimeUtc();
    this->populateTargetFromLocalChannel(target);

    this->currentTarget_ = std::move(target);
    this->heartbeatTimer_.start();

    qCDebug(chatterinoTwitch)
        << "[WatchHeartbeat] Watching" << normalizedLogin
        << "source:" << sourceTag;
    this->targetChanged.invoke();
}

void WatchHeartbeatService::stopWatching(const QString &sourceTag)
{
    if (!this->currentTarget_.has_value())
    {
        return;
    }
    if (!sourceTag.isEmpty() && this->currentTarget_->sourceTag != sourceTag)
    {
        return;
    }

    ++this->generation_;
    const auto prevLogin = this->currentTarget_->login;
    this->currentTarget_.reset();
    this->heartbeatTimer_.stop();
    this->trackedPid_ = 0;
    this->trackedSourceTag_.clear();
    this->processWatchTimer_.stop();

    qCDebug(chatterinoTwitch)
        << "[WatchHeartbeat] Stopped watching" << prevLogin;
    this->targetChanged.invoke();
}

void WatchHeartbeatService::toggleWatching(const QString &channelLogin)
{
    const auto normalizedLogin = channelLogin.trimmed().toLower();
    if (normalizedLogin.isEmpty())
    {
        return;
    }

    if (this->isWatching(normalizedLogin))
    {
        this->stopWatching();
    }
    else
    {
        this->startWatching(normalizedLogin, QStringLiteral("manual"));
    }
}

void WatchHeartbeatService::trackExternalProcess(qint64 pid,
                                                 const QString &channelLogin,
                                                 const QString &sourceTag)
{
    if (pid <= 0)
    {
        return;
    }

    this->trackedPid_ = pid;
    this->trackedSourceTag_ = sourceTag;
    this->startWatching(channelLogin, sourceTag);
    this->processWatchTimer_.start();
}

std::optional<watchheartbeat::detail::WatchTarget>
WatchHeartbeatService::currentTarget() const
{
    return this->currentTarget_;
}

bool WatchHeartbeatService::isWatching(const QString &channelLogin) const
{
    if (!this->currentTarget_.has_value())
    {
        return false;
    }
    return this->currentTarget_->login.compare(channelLogin.trimmed(),
                                               Qt::CaseInsensitive) == 0;
}

void WatchHeartbeatService::stop()
{
    ++this->generation_;
    this->heartbeatTimer_.stop();
    this->processWatchTimer_.stop();
    this->trackedPid_ = 0;
    this->trackedSourceTag_.clear();
    if (this->currentTarget_.has_value())
    {
        this->currentTarget_.reset();
        this->targetChanged.invoke();
    }
}

void WatchHeartbeatService::onProcessWatchTick()
{
    if (this->trackedPid_ <= 0)
    {
        this->processWatchTimer_.stop();
        return;
    }

    if (!isProcessAlive(this->trackedPid_))
    {
        const auto tag = this->trackedSourceTag_;
        this->trackedPid_ = 0;
        this->trackedSourceTag_.clear();
        this->processWatchTimer_.stop();
        this->stopWatching(tag);
    }
}

void WatchHeartbeatService::populateTargetFromLocalChannel(
    watchheartbeat::detail::WatchTarget &target) const
{
    auto *app = tryGetApp();
    if (app == nullptr || app->getTwitch() == nullptr)
    {
        return;
    }

    const auto chan = app->getTwitch()->getChannelOrEmpty(target.login);
    auto *tc = dynamic_cast<TwitchChannel *>(chan.get());
    if (tc == nullptr)
    {
        return;
    }

    if (!tc->roomId().isEmpty())
    {
        target.channelId = tc->roomId();
    }
    const auto status = tc->accessStreamStatus();
    if (status->live)
    {
        target.broadcastId = status->streamId;
        target.gameId = status->gameId;
        target.gameName = status->game;
    }
}

void WatchHeartbeatService::onHeartbeatTick()
{
    if (!getSettings()->enableWatchHeartbeat || !this->currentTarget_.has_value())
    {
        return;
    }

    auto *app = tryGetApp();
    if (app == nullptr)
    {
        return;
    }

    const auto auth = MoltorinoAuth::resolveCurrentUserToken();
    QString userId = auth.userId.trimmed();
    if (userId.isEmpty() && app->getAccounts() != nullptr)
    {
        auto account = app->getAccounts()->twitch.getCurrent();
        if (account && !account->isAnon())
        {
            userId = account->getUserId().trimmed();
        }
    }
    if (!auth.hasToken() || userId.isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "[WatchHeartbeat] Skipping tick: missing auth token or user id";
        return;
    }

    auto target = *this->currentTarget_;
    this->populateTargetFromLocalChannel(target);

    if (!target.channelId.isEmpty() && !target.broadcastId.isEmpty())
    {
        *this->currentTarget_ = target;
        this->sendHeartbeatForTarget(target, userId, auth.token);
        return;
    }

    auto *helix = getHelix();
    if (helix == nullptr)
    {
        return;
    }

    const auto generation = this->generation_;
    const auto token = auth.token;
    helix->fetchStreams(
        {}, {target.login},
        [this, generation, target, userId,
         token](const std::vector<HelixStream> &streams) mutable {
            runInGuiThread([this, generation, target, userId, token,
                            streams]() mutable {
                if (this->generation_ != generation ||
                    !this->currentTarget_.has_value() ||
                    this->currentTarget_->login != target.login)
                {
                    return;
                }

                for (const auto &stream : streams)
                {
                    if (stream.userLogin.compare(target.login,
                                                 Qt::CaseInsensitive) == 0 &&
                        stream.type == QLatin1String("live"))
                    {
                        target.channelId = stream.userId;
                        target.broadcastId = stream.id;
                        target.gameId = stream.gameId;
                        target.gameName = stream.gameName;
                        *this->currentTarget_ = target;
                        this->sendHeartbeatForTarget(target, userId, token);
                        return;
                    }
                }

                qCDebug(chatterinoTwitch)
                    << "[WatchHeartbeat] Skipping tick: channel"
                    << target.login << "is not live";
            });
        },
        [login = target.login]() {
            qCDebug(chatterinoTwitch)
                << "[WatchHeartbeat] Helix fetchStreams failed for" << login;
        },
        [] {});
}

void WatchHeartbeatService::sendHeartbeatForTarget(
    const watchheartbeat::detail::WatchTarget &target, const QString &userId,
    const QString &oauthToken)
{
    const auto legacyB64 =
        watchheartbeat::detail::buildLegacySpadePayload(target, userId);
    const QByteArray legacyBody =
        "data=" + QUrl::toPercentEncoding(QString::fromLatin1(legacyB64));

    NetworkRequest(SPADE_URL, NetworkRequestType::Post)
        .timeout(15000)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .payload(legacyBody)
        .onSuccess([login = target.login](const NetworkResult &) {
            qCDebug(chatterinoTwitch)
                << "[WatchHeartbeat] Legacy spade minute-watched accepted for"
                << login;
        })
        .onError([login = target.login](const NetworkResult &res) {
            qCWarning(chatterinoTwitch)
                << "[WatchHeartbeat] Legacy spade error for" << login << ":"
                << res.formatError();
        })
        .execute();

    if (getSettings()->enableDropsTracking)
    {
        const auto clientTime =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const auto dropsJson = watchheartbeat::detail::buildDropsSpadeJson(
            target, userId, clientTime);

        const QByteArray dropsBody =
            "data=" + QUrl::toPercentEncoding(
                          QString::fromLatin1(dropsJson.toBase64()));
        NetworkRequest(SPADE_URL, NetworkRequestType::Post)
            .timeout(15000)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .payload(dropsBody)
            .onSuccess([login = target.login](const NetworkResult &) {
                qCDebug(chatterinoTwitch)
                    << "[WatchHeartbeat] Drops spade minute-watched accepted for"
                    << login;
            })
            .onError([login = target.login](const NetworkResult &res) {
                qCDebug(chatterinoTwitch)
                    << "[WatchHeartbeat] Drops spade error for" << login << ":"
                    << res.formatError();
            })
            .execute();

        const auto gzB64 = QString::fromLatin1(
            watchheartbeat::detail::gzipCompress(dropsJson).toBase64());
        TwitchGql::sendSpadeEvents(
            gzB64, oauthToken,
            [login = target.login](bool accepted) {
                qCDebug(chatterinoTwitch)
                    << "[WatchHeartbeat] GQL sendSpadeEvents for" << login
                    << "accepted:" << accepted;
            },
            [login = target.login](const QString &err) {
                qCDebug(chatterinoTwitch)
                    << "[WatchHeartbeat] GQL sendSpadeEvents failed for"
                    << login << ":" << err;
            });
    }

    this->heartbeatSent.invoke(target.login);
}

}  // namespace chatterino
