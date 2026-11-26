#pragma once

#include <pajlada/signals/signal.hpp>
#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <optional>
#include <unordered_map>

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

enum class StreamWaitMode {
    IdleMpv,
    Background,
};

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
    void setAutoWatchChannel(const QString &channelLogin, bool isLive);
    void trackExternalProcess(qint64 pid, const QString &channelLogin,
                              const QString &sourceTag);

    void startWaitingForStream(const QString &channelLogin,
                               StreamWaitMode mode);
    void cancelWaitingForStream(const QString &channelLogin);
    bool isWaitingForStream(const QString &channelLogin) const;
    std::optional<StreamWaitMode> waitModeForStream(
        const QString &channelLogin) const;
    void notifyChannelLiveStatus(const QString &channelLogin, bool isLive);

    std::optional<watchheartbeat::detail::WatchTarget> currentTarget() const;
    bool isWatching(const QString &channelLogin) const;
    void stop();

    pajlada::Signals::NoArgSignal targetChanged;
    pajlada::Signals::NoArgSignal waitStateChanged;
    pajlada::Signals::Signal<const QString &> heartbeatSent;

private:
    struct StreamWaitEntry {
        QString login;
        StreamWaitMode mode = StreamWaitMode::IdleMpv;
        qint64 mpvPid = 0;
        QString ipcPipePath;
        bool resolving = false;
    };

    void onHeartbeatTick();
    void onProcessWatchTick();
    void onWaitPollTick();
    void syncProcessWatchTimer();
    void triggerWaitLaunch(const QString &channelLogin);
    void sendHeartbeatForTarget(
        const watchheartbeat::detail::WatchTarget &target,
        const QString &userId, const QString &oauthToken);
    void populateTargetFromLocalChannel(
        watchheartbeat::detail::WatchTarget &target) const;

    std::optional<watchheartbeat::detail::WatchTarget> currentTarget_;
    QString manuallyStoppedLogin_;
    QTimer heartbeatTimer_;
    QTimer processWatchTimer_;
    QTimer waitPollTimer_;
    qint64 trackedPid_ = 0;
    QString trackedSourceTag_;
    std::unordered_map<QString, StreamWaitEntry> waitingStreams_;
    std::uint64_t generation_ = 0;
};

}  // namespace chatterino
