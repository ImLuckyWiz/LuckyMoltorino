#pragma once

#include <pajlada/signals/signalholder.hpp>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_set>

namespace chatterino {

namespace dropplayback::detail {

struct MediaPlaylistResult {
    QVector<QUrl> segments;
    int targetDurationSec = 2;
};

std::optional<QString> lowestVideoVariant(const QString &masterPlaylist);
MediaPlaylistResult parseMediaPlaylist(const QUrl &baseUrl,
                                         const QString &playlistText);

}  // namespace dropplayback::detail

class DropPlaybackService final : public QObject
{
    Q_OBJECT

public:
    explicit DropPlaybackService(QObject *parent = nullptr);
    ~DropPlaybackService() override;

    void initialize();
    void stop();

private:
    void syncWithWatchTarget();
    void resolvePlaylist();
    void pollMediaPlaylist();
    void scheduleRetry();

    bool initialized_ = false;
    std::uint64_t generation_ = 0;
    QString activeLogin_;
    QUrl variantUrl_;
    qint64 resolvedAtMs_ = 0;
    int consecutiveFailures_ = 0;
    QTimer pollTimer_;
    std::unordered_set<QString> seenSegments_;
    std::deque<QString> seenOrder_;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
