#include "providers/twitch/DropsService.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/WatchHeartbeatService.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Toasts.hpp"
#include "util/PostToThread.hpp"

#include <QHash>

namespace chatterino {

namespace {

constexpr int INVENTORY_POLL_INTERVAL_MS = 60 * 1000;

}  // namespace

namespace drops::detail {

bool isDropReadyToClaim(const GqlTimeBasedDrop &drop,
                        const QSet<QString> &claimedDropIds)
{
    if (drop.isClaimed || drop.requiredMinutesWatched <= 0 ||
        drop.currentMinutesWatched < drop.requiredMinutesWatched ||
        drop.dropInstanceId.trimmed().isEmpty())
    {
        return false;
    }

    for (const auto &preId : drop.preconditionDropIds)
    {
        if (!preId.isEmpty() && !claimedDropIds.contains(preId))
        {
            return false;
        }
    }

    return true;
}

QVector<GqlDropCampaign> mergeCampaignsAndInventory(
    const QVector<GqlDropCampaign> &campaigns,
    const QVector<GqlDropCampaign> &inventory)
{
    QHash<QString, const GqlDropCampaign *> inventoryById;
    for (const auto &inv : inventory)
    {
        if (!inv.id.isEmpty())
        {
            inventoryById.insert(inv.id, &inv);
        }
    }

    QVector<GqlDropCampaign> merged;
    merged.reserve(campaigns.size() + inventory.size());
    QSet<QString> seenIds;

    for (auto campaign : campaigns)
    {
        if (!campaign.id.isEmpty())
        {
            seenIds.insert(campaign.id);
        }

        if (const auto *inv = inventoryById.value(campaign.id, nullptr))
        {
            if (inv->isAccountConnected)
            {
                campaign.isAccountConnected = true;
            }
            if (campaign.accountLinkUrl.isEmpty() &&
                !inv->accountLinkUrl.isEmpty())
            {
                campaign.accountLinkUrl = inv->accountLinkUrl;
            }

            QHash<QString, const GqlTimeBasedDrop *> invDropsById;
            for (const auto &d : inv->drops)
            {
                if (!d.id.isEmpty())
                {
                    invDropsById.insert(d.id, &d);
                }
            }

            for (auto &drop : campaign.drops)
            {
                if (const auto *invDrop = invDropsById.value(drop.id, nullptr))
                {
                    if (invDrop->currentMinutesWatched >
                        drop.currentMinutesWatched)
                    {
                        drop.currentMinutesWatched =
                            invDrop->currentMinutesWatched;
                    }
                    if (invDrop->isClaimed)
                    {
                        drop.isClaimed = true;
                    }
                    if (!invDrop->dropInstanceId.isEmpty())
                    {
                        drop.dropInstanceId = invDrop->dropInstanceId;
                    }
                }
            }
        }

        merged.append(std::move(campaign));
    }

    for (const auto &inv : inventory)
    {
        if (!inv.id.isEmpty() && !seenIds.contains(inv.id))
        {
            merged.append(inv);
        }
    }

    return merged;
}

}  // namespace drops::detail

DropsService::DropsService(QObject *parent)
    : QObject(parent)
{
    this->pollTimer_.setInterval(INVENTORY_POLL_INTERVAL_MS);
    this->pollTimer_.setSingleShot(false);
    QObject::connect(&this->pollTimer_, &QTimer::timeout, this,
                     [this] { this->refresh(false); });
}

DropsService::~DropsService()
{
    this->stop();
}

void DropsService::initialize()
{
    if (this->initialized_)
    {
        return;
    }
    this->initialized_ = true;

    auto *app = tryGetApp();
    if (app == nullptr)
    {
        return;
    }

    if (app->getTwitchPubSub() != nullptr)
    {
        this->signalHolder_.managedConnect(
            app->getTwitchPubSub()->userDrops.updated,
            [this](const QJsonObject &payload) {
                runInGuiThread([this, payload] {
                    this->handleUserDropsPubSub(payload);
                });
            });
    }

    if (app->getWatchHeartbeat() != nullptr)
    {
        this->signalHolder_.managedConnect(
            app->getWatchHeartbeat()->targetChanged, [this] {
                runInGuiThread([this] { this->syncPollingTimer(); });
            });
    }

    if (app->getAccounts() != nullptr)
    {
        this->signalHolder_.managedConnect(
            app->getAccounts()->twitch.currentUserChanged, [this] {
                runInGuiThread([this] {
                    this->syncSubscriptions();
                    this->cachedCampaigns_.clear();
                    this->cachedInventory_.clear();
                    this->mergedCampaigns_.clear();
                    this->inFlightClaims_.clear();
                    this->autoClaimedInstanceIds_.clear();
                    this->dropsChanged.invoke();
                });
            });
    }

    getSettings()->enableDropsTracking.connect(
        [this](bool, auto) {
            this->syncSubscriptions();
            this->syncPollingTimer();
        },
        this->signalHolder_, false);

    getSettings()->customPinAuthToken.connect(
        [this](const QString &, auto) { this->syncSubscriptions(); },
        this->signalHolder_, false);

    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) { this->syncSubscriptions(); },
        this->signalHolder_, false);

    this->syncSubscriptions();
    this->syncPollingTimer();
}

void DropsService::stop()
{
    ++this->generation_;
    this->pollTimer_.stop();
    this->loadingCampaigns_ = false;
    this->loadingInventory_ = false;
    this->inFlightClaims_.clear();
}

void DropsService::syncSubscriptions()
{
    auto *app = tryGetApp();
    if (app == nullptr || app->getTwitchPubSub() == nullptr ||
        !getSettings()->enableDropsTracking)
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

    if (auth.hasToken() && !userId.isEmpty())
    {
        app->getTwitchPubSub()->listenToUserDrops(userId, auth.token);
    }
}

void DropsService::syncPollingTimer()
{
    auto *app = tryGetApp();
    const bool hasTarget = app != nullptr && app->getWatchHeartbeat() != nullptr &&
                           app->getWatchHeartbeat()->currentTarget().has_value();

    if (hasTarget && getSettings()->enableDropsTracking)
    {
        if (!this->pollTimer_.isActive())
        {
            this->pollTimer_.start();
            this->refresh(false);
        }
    }
    else
    {
        this->pollTimer_.stop();
    }
}

void DropsService::refresh(bool includeCampaigns)
{
    if (!getSettings()->enableDropsTracking)
    {
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    if (!auth.hasToken())
    {
        this->lastError_ =
            authError.isEmpty()
                ? MoltorinoAuth::authRequiredMessage(
                      QStringLiteral("loading Twitch Drops"))
                : authError;
        this->dropsChanged.invoke();
        return;
    }

    const auto generation = this->generation_;

    if (!this->loadingInventory_)
    {
        this->loadingInventory_ = true;
        this->dropsChanged.invoke();

        TwitchGql::getDropInventory(
            auth.token,
            [this, generation, includeCampaigns](QVector<GqlDropCampaign> inv) {
                runInGuiThread([this, generation, includeCampaigns,
                                inv = std::move(inv)]() mutable {
                    if (this->generation_ != generation)
                    {
                        return;
                    }
                    this->loadingInventory_ = false;
                    this->cachedInventory_ = std::move(inv);
                    this->lastError_.clear();
                    this->finishRefresh(includeCampaigns);
                });
            },
            [this, generation, includeCampaigns](const QString &err) {
                runInGuiThread([this, generation, includeCampaigns, err]() {
                    if (this->generation_ != generation)
                    {
                        return;
                    }
                    this->loadingInventory_ = false;
                    this->lastError_ = MoltorinoAuth::normalizeAuthError(
                        QStringLiteral("loading Twitch Drops"), err);
                    this->finishRefresh(includeCampaigns);
                });
            });
    }

    if (includeCampaigns && !this->loadingCampaigns_)
    {
        this->loadingCampaigns_ = true;
        this->dropsChanged.invoke();

        TwitchGql::getDropCampaigns(
            auth.token,
            [this, generation](QVector<GqlDropCampaign> campaigns) {
                runInGuiThread([this, generation,
                                campaigns = std::move(campaigns)]() mutable {
                    if (this->generation_ != generation)
                    {
                        return;
                    }
                    this->loadingCampaigns_ = false;
                    this->cachedCampaigns_ = std::move(campaigns);
                    this->lastError_.clear();
                    this->finishRefresh(true);
                });
            },
            [this, generation](const QString &err) {
                runInGuiThread([this, generation, err]() {
                    if (this->generation_ != generation)
                    {
                        return;
                    }
                    this->loadingCampaigns_ = false;
                    if (this->lastError_.isEmpty())
                    {
                        this->lastError_ = MoltorinoAuth::normalizeAuthError(
                            QStringLiteral("loading Twitch Drops"), err);
                    }
                    this->finishRefresh(true);
                });
            });
    }
}

void DropsService::finishRefresh(bool /*includeCampaigns*/)
{
    this->mergedCampaigns_ = drops::detail::mergeCampaignsAndInventory(
        this->cachedCampaigns_, this->cachedInventory_);
    this->lastUpdatedAt_ = QDateTime::currentDateTimeUtc();
    this->dropsChanged.invoke();
    this->autoClaimReadyDrops();
}

void DropsService::handleUserDropsPubSub(const QJsonObject &payload)
{
    const auto type = payload.value("type").toString();
    const auto data = payload.value("data").toObject();

    if (type == QLatin1String("drop-progress"))
    {
        const auto dropId = data.value("drop_id").toString();
        const int currentMinutes = data.value("current_progress_min").toInt(-1);
        const int requiredMinutes =
            data.value("required_progress_min").toInt(-1);

        if (!dropId.isEmpty() && currentMinutes >= 0)
        {
            bool updated = false;
            auto apply = [&](QVector<GqlDropCampaign> &list) {
                for (auto &campaign : list)
                {
                    for (auto &drop : campaign.drops)
                    {
                        if (drop.id == dropId)
                        {
                            drop.currentMinutesWatched = std::max(
                                drop.currentMinutesWatched, currentMinutes);
                            if (requiredMinutes > 0)
                            {
                                drop.requiredMinutesWatched = requiredMinutes;
                            }
                            updated = true;
                        }
                    }
                }
            };
            apply(this->cachedInventory_);
            apply(this->cachedCampaigns_);
            apply(this->mergedCampaigns_);
            if (updated)
            {
                this->dropsChanged.invoke();
            }
        }
    }
    else if (type == QLatin1String("drop-claim"))
    {
        const auto dropId = data.value("drop_id").toString();
        const auto instanceId = data.value("drop_instance_id").toString();
        if (!dropId.isEmpty() && !instanceId.isEmpty())
        {
            auto apply = [&](QVector<GqlDropCampaign> &list) {
                for (auto &campaign : list)
                {
                    for (auto &drop : campaign.drops)
                    {
                        if (drop.id == dropId)
                        {
                            drop.dropInstanceId = instanceId;
                            if (drop.requiredMinutesWatched > 0)
                            {
                                drop.currentMinutesWatched =
                                    std::max(drop.currentMinutesWatched,
                                             drop.requiredMinutesWatched);
                            }
                        }
                    }
                }
            };
            apply(this->cachedInventory_);
            apply(this->cachedCampaigns_);
            apply(this->mergedCampaigns_);
            this->dropsChanged.invoke();
            this->autoClaimReadyDrops();
        }
        this->refresh(false);
    }
}

void DropsService::autoClaimReadyDrops()
{
    if (!getSettings()->autoClaimDrops)
    {
        return;
    }

    for (const auto &campaign : this->mergedCampaigns_)
    {
        QSet<QString> claimedIds;
        for (const auto &drop : campaign.drops)
        {
            if (drop.isClaimed && !drop.id.isEmpty())
            {
                claimedIds.insert(drop.id);
            }
        }

        for (const auto &drop : campaign.drops)
        {
            if (drops::detail::isDropReadyToClaim(drop, claimedIds) &&
                !this->autoClaimedInstanceIds_.contains(drop.dropInstanceId))
            {
                this->autoClaimedInstanceIds_.insert(drop.dropInstanceId);
                QString label = drop.name;
                if (label.isEmpty() && !drop.benefits.isEmpty())
                {
                    label = drop.benefits.first().name;
                }
                this->claimDrop(drop.dropInstanceId, label, true);
            }
        }
    }
}

void DropsService::claimDrop(const QString &dropInstanceId,
                             const QString &dropName, bool automatic)
{
    const auto trimmedId = dropInstanceId.trimmed();
    if (trimmedId.isEmpty() || this->inFlightClaims_.contains(trimmedId))
    {
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    if (!auth.hasToken())
    {
        if (!automatic)
        {
            this->lastError_ = authError;
            this->dropsChanged.invoke();
        }
        return;
    }

    this->inFlightClaims_.insert(trimmedId);
    const auto generation = this->generation_;

    TwitchGql::claimDropReward(
        trimmedId, auth.token,
        [this, generation, trimmedId, dropName,
         automatic](GqlDropClaimOutcome outcome) {
            runInGuiThread([this, generation, trimmedId, dropName, automatic,
                            outcome]() {
                this->inFlightClaims_.remove(trimmedId);
                if (this->generation_ != generation)
                {
                    return;
                }

                auto markClaimed = [&](QVector<GqlDropCampaign> &list) {
                    for (auto &campaign : list)
                    {
                        for (auto &drop : campaign.drops)
                        {
                            if (drop.dropInstanceId == trimmedId)
                            {
                                drop.isClaimed = true;
                            }
                        }
                    }
                };
                markClaimed(this->cachedInventory_);
                markClaimed(this->cachedCampaigns_);
                markClaimed(this->mergedCampaigns_);
                this->dropsChanged.invoke();

                if (outcome == GqlDropClaimOutcome::Claimed)
                {
                    const QString display = dropName.isEmpty()
                                                ? QStringLiteral("Twitch Drop")
                                                : dropName;
                    watchheartbeat::appendWatchPointsLog(
                        QStringLiteral("Drops"),
                        QStringLiteral("Claimed %1 (%2)")
                            .arg(display,
                                 automatic ? QStringLiteral("auto")
                                           : QStringLiteral("manual")));
                    auto *app = tryGetApp();
                    if (app != nullptr && app->getWatchHeartbeat() != nullptr &&
                        app->getTwitch() != nullptr)
                    {
                        if (const auto target =
                                app->getWatchHeartbeat()->currentTarget())
                        {
                            const auto chan =
                                app->getTwitch()->getChannelOrEmpty(target->login);
                            if (chan && !chan->isEmpty())
                            {
                                chan->addSystemMessage(
                                    QStringLiteral("Claimed Twitch Drop: %1")
                                        .arg(display));
                            }
                        }
                    }

                    if ((!automatic || getSettings()->notifyDropsClaimed) &&
                        app != nullptr && app->getToasts() != nullptr)
                    {
                        app->getToasts()->sendHighlightNotification(
                            QStringLiteral("Twitch Drops"),
                            QStringLiteral("Drop Claimed"), display,
                            QStringLiteral("drop:%1").arg(trimmedId));
                    }
                }

                this->refresh(false);
            });
        },
        [this, generation, trimmedId, automatic](const QString &err) {
            runInGuiThread([this, generation, trimmedId, automatic, err]() {
                this->inFlightClaims_.remove(trimmedId);
                if (this->generation_ != generation)
                {
                    return;
                }
                qCWarning(chatterinoTwitch)
                    << "[Drops] Claim failed for" << trimmedId << ":" << err;
                if (!automatic)
                {
                    this->lastError_ = err;
                    this->dropsChanged.invoke();
                }
            });
        });
}

QVector<GqlDropCampaign> DropsService::campaigns() const
{
    return this->mergedCampaigns_;
}

bool DropsService::isLoading() const
{
    return this->loadingCampaigns_ || this->loadingInventory_;
}

QString DropsService::lastError() const
{
    return this->lastError_;
}

QDateTime DropsService::lastUpdatedAt() const
{
    return this->lastUpdatedAt_;
}

}  // namespace chatterino
