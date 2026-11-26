#include "providers/twitch/DropPlaybackService.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/WatchHeartbeatService.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QDateTime>
#include <QRandomGenerator>
#include <QUrlQuery>

#include <algorithm>
#include <limits>

namespace chatterino {

namespace {

constexpr qint64 RESOLVE_EVERY_MS = 10 * 60 * 1000;
constexpr size_t SEEN_SEGMENTS_CAP = 256;

}  // namespace

namespace dropplayback::detail {

std::optional<QString> lowestVideoVariant(const QString &masterPlaylist)
{
    const QStringList lines = masterPlaylist.split('\n');
    std::optional<quint64> bestBandwidth;
    QString bestUrl;

    for (qsizetype i = 0; i < lines.size(); ++i)
    {
        const QString line = lines.at(i).trimmed();
        if (!line.startsWith(QLatin1String("#EXT-X-STREAM-INF:")))
        {
            continue;
        }

        const QString attrs =
            line.mid(qsizetype(sizeof("#EXT-X-STREAM-INF:") - 1));
        if (attrs.contains(QLatin1String("audio_only")))
        {
            continue;
        }

        quint64 bandwidth = std::numeric_limits<quint64>::max();
        const QStringList parts = attrs.split(',');
        for (const auto &part : parts)
        {
            const auto trimmedPart = part.trimmed();
            if (trimmedPart.startsWith(QLatin1String("BANDWIDTH=")))
            {
                bool ok = false;
                const auto parsed =
                    trimmedPart.mid(qsizetype(sizeof("BANDWIDTH=") - 1))
                        .toULongLong(&ok);
                if (ok)
                {
                    bandwidth = parsed;
                }
                break;
            }
        }

        if (i + 1 >= lines.size())
        {
            continue;
        }

        const QString urlLine = lines.at(i + 1).trimmed();
        if (urlLine.isEmpty() || urlLine.startsWith('#'))
        {
            continue;
        }

        if (!bestBandwidth.has_value() || bandwidth < *bestBandwidth)
        {
            bestBandwidth = bandwidth;
            bestUrl = urlLine;
        }
    }

    if (bestUrl.isEmpty())
    {
        return std::nullopt;
    }
    return bestUrl;
}

MediaPlaylistResult parseMediaPlaylist(const QUrl &baseUrl,
                                         const QString &playlistText)
{
    MediaPlaylistResult result;
    const QStringList lines = playlistText.split('\n');

    for (const auto &rawLine : lines)
    {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
        {
            continue;
        }
        if (line.startsWith(QLatin1String("#EXT-X-TARGETDURATION:")))
        {
            bool ok = false;
            const double val =
                line.mid(qsizetype(sizeof("#EXT-X-TARGETDURATION:") - 1))
                    .trimmed()
                    .toDouble(&ok);
            if (ok && val > 0.0)
            {
                int sec = int(std::ceil(val));
                result.targetDurationSec = std::clamp(sec, 1, 10);
            }
            continue;
        }
        if (line.startsWith('#'))
        {
            continue;
        }

        const QUrl resolved = baseUrl.resolved(QUrl(line));
        if (resolved.isValid())
        {
            result.segments.append(resolved);
        }
    }

    return result;
}

}  // namespace dropplayback::detail

DropPlaybackService::DropPlaybackService(QObject *parent)
    : QObject(parent)
{
    this->pollTimer_.setSingleShot(true);
    QObject::connect(&this->pollTimer_, &QTimer::timeout, this,
                     &DropPlaybackService::pollMediaPlaylist);
}

DropPlaybackService::~DropPlaybackService()
{
    this->stop();
}

void DropPlaybackService::initialize()
{
    if (this->initialized_)
    {
        return;
    }
    this->initialized_ = true;

    if (auto *app = tryGetApp(); app && app->getWatchHeartbeat())
    {
        this->signalHolder_.managedConnect(
            app->getWatchHeartbeat()->targetChanged, [this] {
                runInGuiThread([this] { this->syncWithWatchTarget(); });
            });
    }

    getSettings()->enableDropsTracking.connect(
        [this](bool, auto) { this->syncWithWatchTarget(); },
        this->signalHolder_, false);

    this->syncWithWatchTarget();
}

void DropPlaybackService::stop()
{
    ++this->generation_;
    this->pollTimer_.stop();
    this->activeLogin_.clear();
    this->variantUrl_.clear();
    this->resolvedAtMs_ = 0;
    this->consecutiveFailures_ = 0;
    this->seenSegments_.clear();
    this->seenOrder_.clear();
}

void DropPlaybackService::syncWithWatchTarget()
{
    auto *app = tryGetApp();
    if (app == nullptr || app->getWatchHeartbeat() == nullptr ||
        !getSettings()->enableDropsTracking)
    {
        this->stop();
        return;
    }

    const auto target = app->getWatchHeartbeat()->currentTarget();
    if (!target.has_value())
    {
        this->stop();
        return;
    }

    if (target->login == this->activeLogin_ && this->variantUrl_.isValid())
    {
        return;
    }

    ++this->generation_;
    this->pollTimer_.stop();
    this->activeLogin_ = target->login;
    this->variantUrl_.clear();
    this->resolvedAtMs_ = 0;
    this->consecutiveFailures_ = 0;
    this->seenSegments_.clear();
    this->seenOrder_.clear();

    this->resolvePlaylist();
}

void DropPlaybackService::resolvePlaylist()
{
    if (this->activeLogin_.isEmpty() || !getSettings()->enableDropsTracking)
    {
        return;
    }

    const auto auth = MoltorinoAuth::resolveCurrentUserToken();
    if (!auth.hasToken())
    {
        this->scheduleRetry();
        return;
    }

    const auto generation = this->generation_;
    const auto login = this->activeLogin_;

    TwitchGql::getPlaybackAccessToken(
        login, auth.token,
        [this, generation, login](GqlPlaybackAccessToken pat) {
            runInGuiThread([this, generation, login, pat]() {
                if (this->generation_ != generation ||
                    this->activeLogin_ != login)
                {
                    return;
                }

                QUrl usherUrl(
                    QStringLiteral("https://usher.ttvnw.net/api/channel/hls/%1.m3u8")
                        .arg(login));
                QUrlQuery query;
                query.addQueryItem(QStringLiteral("platform"),
                                   QStringLiteral("web"));
                query.addQueryItem(QStringLiteral("player_type"),
                                   QStringLiteral("site"));
                query.addQueryItem(QStringLiteral("allow_source"),
                                   QStringLiteral("true"));
                query.addQueryItem(QStringLiteral("playlist_include_framerate"),
                                   QStringLiteral("true"));
                query.addQueryItem(
                    QStringLiteral("p"),
                    QString::number(
                        QRandomGenerator::global()->bounded(1000000, 9999999)));
                query.addQueryItem(QStringLiteral("sig"), pat.signature);
                query.addQueryItem(QStringLiteral("token"), pat.value);
                usherUrl.setQuery(query);

                NetworkRequest(usherUrl, NetworkRequestType::Get)
                    .timeout(10000)
                    .maximumResponseSize(2 * 1024 * 1024)
                    .onSuccess([this, generation,
                                login](const NetworkResult &result) {
                        runInGuiThread([this, generation, login,
                                        text = QString::fromUtf8(
                                            result.getData())]() {
                            if (this->generation_ != generation ||
                                this->activeLogin_ != login)
                            {
                                return;
                            }

                            const auto variant =
                                dropplayback::detail::lowestVideoVariant(text);
                            if (!variant.has_value())
                            {
                                qCDebug(chatterinoTwitch)
                                    << "[DropPlayback] No video rendition for"
                                    << login;
                                this->scheduleRetry();
                                return;
                            }

                            this->variantUrl_ = QUrl(*variant);
                            this->resolvedAtMs_ =
                                QDateTime::currentMSecsSinceEpoch();
                            this->consecutiveFailures_ = 0;
                            qCDebug(chatterinoTwitch)
                                << "[DropPlayback] Following" << login;
                            this->pollMediaPlaylist();
                        });
                    })
                    .onError([this, generation,
                              login](const NetworkResult &result) {
                        runInGuiThread([this, generation, login,
                                        err = result.formatError()]() {
                            if (this->generation_ != generation ||
                                this->activeLogin_ != login)
                            {
                                return;
                            }
                            qCDebug(chatterinoTwitch)
                                << "[DropPlayback] Usher failed for" << login
                                << ":" << err;
                            this->scheduleRetry();
                        });
                    })
                    .execute();
            });
        },
        [this, generation, login](const QString &error) {
            runInGuiThread([this, generation, login, error]() {
                if (this->generation_ != generation ||
                    this->activeLogin_ != login)
                {
                    return;
                }
                qCDebug(chatterinoTwitch)
                    << "[DropPlayback] PlaybackAccessToken failed for" << login
                    << ":" << error;
                this->scheduleRetry();
            });
        });
}

void DropPlaybackService::pollMediaPlaylist()
{
    if (this->activeLogin_.isEmpty() || !this->variantUrl_.isValid() ||
        !getSettings()->enableDropsTracking)
    {
        return;
    }

    const auto nowMs = QDateTime::currentMSecsSinceEpoch();
    if (this->resolvedAtMs_ <= 0 ||
        nowMs - this->resolvedAtMs_ >= RESOLVE_EVERY_MS)
    {
        this->resolvePlaylist();
        return;
    }

    const auto generation = this->generation_;
    const auto login = this->activeLogin_;
    const auto baseUrl = this->variantUrl_;

    NetworkRequest(baseUrl, NetworkRequestType::Get)
        .timeout(10000)
        .maximumResponseSize(2 * 1024 * 1024)
        .onSuccess([this, generation, login, baseUrl](const NetworkResult &res) {
            runInGuiThread([this, generation, login, baseUrl,
                            text = QString::fromUtf8(res.getData())]() {
                if (this->generation_ != generation ||
                    this->activeLogin_ != login)
                {
                    return;
                }

                const auto parsed =
                    dropplayback::detail::parseMediaPlaylist(baseUrl, text);
                for (const auto &segUrl : parsed.segments)
                {
                    const auto key = segUrl.toString();
                    if (!this->seenSegments_.insert(key).second)
                    {
                        continue;
                    }
                    this->seenOrder_.push_back(key);
                    while (this->seenOrder_.size() > SEEN_SEGMENTS_CAP)
                    {
                        this->seenSegments_.erase(this->seenOrder_.front());
                        this->seenOrder_.pop_front();
                    }

                    NetworkRequest(segUrl, NetworkRequestType::Head)
                        .timeout(10000)
                        .execute();
                }

                this->consecutiveFailures_ = 0;
                this->pollTimer_.start(parsed.targetDurationSec * 1000);
            });
        })
        .onError([this, generation, login](const NetworkResult &res) {
            runInGuiThread([this, generation, login,
                            err = res.formatError()]() {
                if (this->generation_ != generation ||
                    this->activeLogin_ != login)
                {
                    return;
                }
                qCDebug(chatterinoTwitch)
                    << "[DropPlayback] Media playlist error for" << login << ":"
                    << err;
                this->variantUrl_.clear();
                this->scheduleRetry();
            });
        })
        .execute();
}

void DropPlaybackService::scheduleRetry()
{
    ++this->consecutiveFailures_;
    const int shift = std::min(this->consecutiveFailures_, 4);
    const int waitSec = std::min(300, 15 * (1 << shift));
    const auto generation = this->generation_;
    const auto login = this->activeLogin_;
    QTimer::singleShot(waitSec * 1000, this, [this, generation, login] {
        if (this->generation_ == generation && this->activeLogin_ == login)
        {
            this->resolvePlaylist();
        }
    });
}

}  // namespace chatterino
