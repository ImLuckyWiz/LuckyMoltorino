#pragma once

#include <pajlada/signals/signal.hpp>
#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <optional>

namespace chatterino {

namespace watchheartbeat::detail {

struct WatchTarget {
    QString channelId;
    QString login;
    QString broadcastId;
    QString gameId;
    QString gameName;
    QString sourceTag;
    QDateTime startedAt;
};

QByteArray buildLegacySpadePayload(const WatchTarget &target,
                                   const QString &userId);
QByteArray buildDropsSpadeJson(const WatchTarget &target, const QString &userId,
                               const QString &clientTimeIso);
QByteArray gzipCompress(const QByteArray &input);
std::optional<QString> extractTwitchLoginFromUrl(const QString &urlOrTarget);

}  // namespace watchheartbeat::detail

class WatchHeartbeatService final : public QObject
{
    Q_OBJECT

public:
    explicit WatchHeartbeatService(QObject *parent = nullptr);
    ~WatchHeartbeatService() override;

    void startWatching(const QString &channelLogin,
                       const QString &sourceTag = QStringLiteral("manual"));
    void stopWatching(const QString &sourceTag = {});
    void toggleWatching(const QString &channelLogin);
    void trackExternalProcess(qint64 pid, const QString &channelLogin,
                              const QString &sourceTag);

    std::optional<watchheartbeat::detail::WatchTarget> currentTarget() const;
    bool isWatching(const QString &channelLogin) const;
    void stop();

    pajlada::Signals::NoArgSignal targetChanged;
    pajlada::Signals::Signal<const QString &> heartbeatSent;

private:
    void onHeartbeatTick();
    void onProcessWatchTick();
    void sendHeartbeatForTarget(
        const watchheartbeat::detail::WatchTarget &target,
        const QString &userId, const QString &oauthToken);
    void populateTargetFromLocalChannel(
        watchheartbeat::detail::WatchTarget &target) const;

    std::optional<watchheartbeat::detail::WatchTarget> currentTarget_;
    QTimer heartbeatTimer_;
    QTimer processWatchTimer_;
    qint64 trackedPid_ = 0;
    QString trackedSourceTag_;
    std::uint64_t generation_ = 0;
};

}  // namespace chatterino
