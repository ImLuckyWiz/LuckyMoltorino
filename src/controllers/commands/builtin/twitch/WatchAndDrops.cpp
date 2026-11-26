#include "controllers/commands/builtin/twitch/WatchAndDrops.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/WatchHeartbeatService.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/DropsDialog.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

namespace chatterino::commands {

namespace {

Split *findOpenSplitForChannel(const ChannelPtr &channel)
{
    if (!channel)
    {
        return nullptr;
    }

    auto *app = tryGetApp();
    if (app == nullptr)
    {
        return nullptr;
    }

    auto *window = app->getWindows()->getLastSelectedWindow();
    if (window == nullptr)
    {
        return nullptr;
    }

    auto *page = window->getNotebook().getSelectedPage();
    if (auto *container = dynamic_cast<SplitContainer *>(page))
    {
        for (auto *split : container->getSplits())
        {
            if (split != nullptr &&
                (split->getChannel() == channel ||
                 split->getSelectedChannel() == channel))
            {
                return split;
            }
        }
    }

    return nullptr;
}

}  // namespace

QString openDropsDialog(const CommandContext &ctx)
{
    DropsDialog::showDialog(findOpenSplitForChannel(ctx.channel));
    return {};
}

QString toggleWatchChannel(const CommandContext &ctx)
{
    auto *app = tryGetApp();
    if (app == nullptr || app->getWatchHeartbeat() == nullptr)
    {
        return {};
    }

    auto *heartbeat = app->getWatchHeartbeat();

    if (ctx.words.size() >= 2)
    {
        const auto arg = ctx.words.at(1).trimmed().toLower();
        if (arg == QLatin1String("off") || arg == QLatin1String("stop") ||
            arg == QLatin1String("clear"))
        {
            heartbeat->stopWatching();
            if (ctx.channel)
            {
                ctx.channel->addSystemMessage(
                    QStringLiteral("Stopped watch-time & Drops tracking."));
            }
            return {};
        }

        heartbeat->startWatching(arg, QStringLiteral("manual"));
        if (ctx.channel)
        {
            ctx.channel->addSystemMessage(
                QStringLiteral("Now tracking watch-time & Drops for %1.")
                    .arg(arg));
        }
        return {};
    }

    if (ctx.twitchChannel == nullptr)
    {
        if (ctx.channel)
        {
            ctx.channel->addSystemMessage(
                QStringLiteral("Usage: /watch <channel|off> (or run /watch "
                               "inside a Twitch channel)."));
        }
        return {};
    }

    const auto login = ctx.twitchChannel->getName();
    if (heartbeat->isWatching(login))
    {
        heartbeat->stopWatching();
        ctx.channel->addSystemMessage(
            QStringLiteral("Stopped watch-time & Drops tracking for %1.")
                .arg(login));
    }
    else
    {
        heartbeat->startWatching(login, QStringLiteral("manual"));
        ctx.channel->addSystemMessage(
            QStringLiteral("Now tracking watch-time & Drops for %1.")
                .arg(login));
    }

    return {};
}

QString claimChannelPointsBonus(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        if (ctx.channel)
        {
            ctx.channel->addSystemMessage(QStringLiteral(
                "The /claim command only works in Twitch channels."));
        }
        return {};
    }

    if (!ctx.twitchChannel->hasAvailableBonusClaim())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("No bonus chest available to claim."));
        return {};
    }

    if (ctx.twitchChannel->isBonusClaimInFlight())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Bonus chest claim is already in progress."));
        return {};
    }

    ctx.twitchChannel->claimAvailableBonusPoints(false);
    return {};
}

QString waitForStreamCommand(const CommandContext &ctx)
{
    auto *app = tryGetApp();
    if (app == nullptr || app->getWatchHeartbeat() == nullptr)
    {
        return {};
    }
    if (ctx.twitchChannel == nullptr)
    {
        if (ctx.channel)
        {
            ctx.channel->addSystemMessage(QStringLiteral(
                "The /watchwait command only works in Twitch channels."));
        }
        return {};
    }

    auto *hb = app->getWatchHeartbeat();
    const auto login = ctx.twitchChannel->getName();
    const auto arg = ctx.words.size() >= 2
                         ? ctx.words.at(1).trimmed().toLower()
                         : QStringLiteral("idle");

    if (arg == QLatin1String("off") || arg == QLatin1String("cancel") ||
        arg == QLatin1String("stop"))
    {
        hb->cancelWaitingForStream(login);
    }
    else if (arg == QLatin1String("bg") ||
             arg == QLatin1String("background"))
    {
        hb->startWaitingForStream(login, StreamWaitMode::Background);
    }
    else if (arg == QLatin1String("idle") || arg == QLatin1String("mpv"))
    {
        hb->startWaitingForStream(login, StreamWaitMode::IdleMpv);
    }
    else
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Usage: /watchwait [idle|bg|off]"));
    }

    return {};
}

QString openWatchPointsLogCommand(const CommandContext &ctx)
{
    if (!ctx.channel)
    {
        return {};
    }

    const QString path = watchheartbeat::watchPointsLogFilePath();
    if (path.isEmpty())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Watch/Points log path is unavailable."));
        return {};
    }

    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!QFileInfo::exists(path))
    {
        QFile file(path);
        file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }

    ctx.channel->addSystemMessage(
        QStringLiteral("Watch & Points log: %1").arg(QDir::toNativeSeparators(path)));
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    return {};
}

}  // namespace chatterino::commands
