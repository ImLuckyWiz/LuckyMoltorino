#pragma once

#include "providers/twitch/api/TwitchGql.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

#include <cstdint>

namespace chatterino {

namespace drops::detail {

bool isDropReadyToClaim(const GqlTimeBasedDrop &drop,
                        const QSet<QString> &claimedDropIds);

QVector<GqlDropCampaign> mergeCampaignsAndInventory(
    const QVector<GqlDropCampaign> &campaigns,
    const QVector<GqlDropCampaign> &inventory);

}  // namespace drops::detail

class DropsService final : public QObject
{
    Q_OBJECT

public:
    explicit DropsService(QObject *parent = nullptr);
    ~DropsService() override;

    void initialize();
    void stop();

    void refresh(bool includeCampaigns = true);
    void claimDrop(const QString &dropInstanceId, const QString &dropName,
                   bool automatic = false);

    QVector<GqlDropCampaign> campaigns() const;
    bool isLoading() const;
    QString lastError() const;
    QDateTime lastUpdatedAt() const;

    pajlada::Signals::NoArgSignal dropsChanged;

private:
    void syncSubscriptions();
    void syncPollingTimer();
    void handleUserDropsPubSub(const QJsonObject &payload);
    void autoClaimReadyDrops();
    void finishRefresh(bool includeCampaigns);

    bool initialized_ = false;
    std::uint64_t generation_ = 0;
    bool loadingCampaigns_ = false;
    bool loadingInventory_ = false;
    QString lastError_;
    QDateTime lastUpdatedAt_;
    QVector<GqlDropCampaign> cachedCampaigns_;
    QVector<GqlDropCampaign> cachedInventory_;
    QVector<GqlDropCampaign> mergedCampaigns_;
    QSet<QString> inFlightClaims_;
    QSet<QString> autoClaimedInstanceIds_;
    QTimer pollTimer_;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
