#include "providers/twitch/DropPlaybackService.hpp"
#include "providers/twitch/DropsService.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/WatchHeartbeatService.hpp"

#include "common/Args.hpp"
#include "common/Modes.hpp"
#include "common/network/NetworkManager.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "mocks/DisabledStreamerMode.hpp"
#include "mocks/EmptyApplication.hpp"
#include "mocks/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "mocks/TwitchUsers.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/Updates.hpp"
#include "Test.hpp"

#include <boost/beast/zlib/inflate_stream.hpp>
#include <boost/crc.hpp>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>

namespace chatterino {

namespace {

class WatchDropsMockApplication : public mock::EmptyApplication
{
public:
    explicit WatchDropsMockApplication(Settings &settings)
        : updates(this->paths_, settings)
        , theme(this->paths_)
        , fonts(settings)
        , pubSub("wss://127.0.0.1:9050")
    {
    }

    Updates &getUpdates() override
    {
        return this->updates;
    }

    IStreamerMode *getStreamerMode() override
    {
        return &this->streamerMode;
    }

    Theme *getThemes() override
    {
        return &this->theme;
    }

    Fonts *getFonts() override
    {
        return &this->fonts;
    }

    ITwitchUsers *getTwitchUsers() override
    {
        return &this->twitchUsers;
    }

    BttvLiveUpdates *getBttvLiveUpdates() override
    {
        return nullptr;
    }

    SeventvEventAPI *getSeventvEventAPI() override
    {
        return nullptr;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    PubSub *getTwitchPubSub() override
    {
        return &this->pubSub;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    Updates updates;
    ::DisabledStreamerMode streamerMode;
    Theme theme;
    Fonts fonts;
    mock::TwitchUsers twitchUsers;
    AccountController accounts;
    PubSub pubSub;
    mock::MockTwitchIrcServer twitch;
    mock::Logging logging;
};

}  // namespace

class TwitchChannelTestAccess
{
public:
    static void setRoomId(TwitchChannel &channel, const QString &roomId)
    {
        channel.setRoomId(roomId);
    }
};

TEST(WatchHeartbeat, LegacySpadePayload)
{
    watchheartbeat::detail::WatchTarget target;
    target.channelId = QStringLiteral("12345");
    target.login = QStringLiteral("streamer");
    target.broadcastId = QStringLiteral("98765");

    const auto b64 = watchheartbeat::detail::buildLegacySpadePayload(
        target, QStringLiteral("55555"));
    const auto raw = QByteArray::fromBase64(b64);
    const auto doc = QJsonDocument::fromJson(raw);
    ASSERT_TRUE(doc.isArray());
    const auto arr = doc.array();
    ASSERT_EQ(arr.size(), 1);

    const auto eventObj = arr.at(0).toObject();
    EXPECT_EQ(eventObj.value("event").toString(),
              QStringLiteral("minute-watched"));

    const auto props = eventObj.value("properties").toObject();
    EXPECT_EQ(props.value("broadcast_id").toString(), QStringLiteral("98765"));
    EXPECT_EQ(props.value("channel_id").toString(), QStringLiteral("12345"));
    EXPECT_EQ(props.value("channel").toString(), QStringLiteral("streamer"));
    EXPECT_EQ(props.value("location").toString(), QStringLiteral("channel"));
    EXPECT_EQ(props.value("player").toString(), QStringLiteral("site"));
    EXPECT_EQ(props.value("user_id").toString(), QStringLiteral("55555"));
    EXPECT_TRUE(props.value("live").toBool());
    EXPECT_TRUE(props.value("logged_in").toBool());
    EXPECT_FALSE(props.value("hidden").toBool());
    EXPECT_FALSE(props.value("muted").toBool());
}

TEST(WatchHeartbeat, GzipRoundtrip)
{
    watchheartbeat::detail::WatchTarget target;
    target.channelId = QStringLiteral("12345");
    target.login = QStringLiteral("streamer");
    target.broadcastId = QStringLiteral("98765");
    target.gameId = QStringLiteral("509658");
    target.gameName = QStringLiteral("Just Chatting");

    const auto input = watchheartbeat::detail::buildDropsSpadeJson(
        target, QStringLiteral("55555"),
        QStringLiteral("2026-11-26T12:00:00.000Z"));
    const auto gz = watchheartbeat::detail::gzipCompress(input);

    ASSERT_GT(gz.size(), 18);
    EXPECT_EQ(static_cast<unsigned char>(gz.at(0)), 0x1f);
    EXPECT_EQ(static_cast<unsigned char>(gz.at(1)), 0x8b);
    EXPECT_EQ(static_cast<unsigned char>(gz.at(2)), 0x08);

    const qsizetype deflateStart = 10;
    const qsizetype deflateEnd = gz.size() - 8;

    QByteArray inflated(input.size() + 64, Qt::Uninitialized);
    boost::beast::zlib::inflate_stream stream;
    boost::beast::zlib::z_params params{};
    params.next_in = gz.constData() + deflateStart;
    params.avail_in = size_t(deflateEnd - deflateStart);
    params.next_out = inflated.data();
    params.avail_out = size_t(inflated.size());
    boost::system::error_code ec;
    stream.write(params, boost::beast::zlib::Flush::finish, ec);
    EXPECT_EQ(ec, boost::beast::zlib::error::end_of_stream);
    inflated.resize(qsizetype(params.total_out));
    EXPECT_EQ(inflated, input);

    boost::crc_32_type crc;
    crc.process_bytes(input.constData(), size_t(input.size()));
    EXPECT_EQ(qFromLittleEndian<quint32>(gz.constData() + deflateEnd),
              quint32(crc.checksum()));
    EXPECT_EQ(qFromLittleEndian<quint32>(gz.constData() + deflateEnd + 4),
              quint32(input.size()));
}

TEST(WatchHeartbeat, ExtractTwitchLogin)
{
    EXPECT_EQ(watchheartbeat::detail::extractTwitchLoginFromUrl(
                  QStringLiteral("https://www.twitch.tv/Forsen")),
              QStringLiteral("forsen"));
    EXPECT_EQ(watchheartbeat::detail::extractTwitchLoginFromUrl(
                  QStringLiteral("twitch.tv/nymn/")),
              QStringLiteral("nymn"));
    EXPECT_EQ(watchheartbeat::detail::extractTwitchLoginFromUrl(
                  QStringLiteral("xqc")),
              QStringLiteral("xqc"));
    EXPECT_FALSE(watchheartbeat::detail::extractTwitchLoginFromUrl(
                     QStringLiteral("https://kick.com/xqc"))
                     .has_value());
    EXPECT_FALSE(watchheartbeat::detail::extractTwitchLoginFromUrl(
                     QStringLiteral("https://twitch.tv/directory/following"))
                     .has_value());
}

TEST(DropPlayback, LowestVideoVariant)
{
    const QString master = QStringLiteral(
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=6000000,RESOLUTION=1920x1080,VIDEO=\"chunked\"\n"
        "https://a.example/chunked.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=160000,VIDEO=\"audio_only\"\n"
        "https://a.example/audio.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=230000,RESOLUTION=284x160,VIDEO=\"160p30\"\n"
        "https://a.example/160p.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=630000,RESOLUTION=640x360,VIDEO=\"360p30\"\n"
        "https://a.example/360p.m3u8\n");

    EXPECT_EQ(dropplayback::detail::lowestVideoVariant(master),
              QStringLiteral("https://a.example/160p.m3u8"));
}

TEST(DropPlayback, ParseMediaPlaylist)
{
    const QUrl base(QStringLiteral("https://b.example/v1/playlist/x.m3u8"));
    const QString text = QStringLiteral(
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-TWITCH-PREFETCH:https://b.example/p.ts\n"
        "#EXTINF:2.000,live\n"
        "https://b.example/seg1.ts\n"
        "#EXTINF:2.000,live\n"
        "seg2.ts\n");

    const auto res = dropplayback::detail::parseMediaPlaylist(base, text);
    EXPECT_EQ(res.targetDurationSec, 6);
    ASSERT_EQ(res.segments.size(), 2);
    EXPECT_EQ(res.segments.at(0).toString(),
              QStringLiteral("https://b.example/seg1.ts"));
    EXPECT_EQ(res.segments.at(1).toString(),
              QStringLiteral("https://b.example/v1/playlist/seg2.ts"));

    const auto emptyRes = dropplayback::detail::parseMediaPlaylist(
        base, QStringLiteral("#EXTM3U\n"));
    EXPECT_EQ(emptyRes.targetDurationSec, 2);
    EXPECT_TRUE(emptyRes.segments.isEmpty());
}

TEST(Drops, ReadyToClaimAndPreconditions)
{
    GqlTimeBasedDrop drop;
    drop.id = QStringLiteral("d2");
    drop.requiredMinutesWatched = 60;
    drop.currentMinutesWatched = 60;
    drop.dropInstanceId = QStringLiteral("inst-2");
    drop.preconditionDropIds = {QStringLiteral("d1")};

    QSet<QString> claimed;
    EXPECT_FALSE(drops::detail::isDropReadyToClaim(drop, claimed));

    claimed.insert(QStringLiteral("d1"));
    EXPECT_TRUE(drops::detail::isDropReadyToClaim(drop, claimed));

    drop.isClaimed = true;
    EXPECT_FALSE(drops::detail::isDropReadyToClaim(drop, claimed));

    drop.isClaimed = false;
    drop.dropInstanceId.clear();
    EXPECT_FALSE(drops::detail::isDropReadyToClaim(drop, claimed));
}

TEST(Drops, MergeCampaignsAndInventory)
{
    GqlDropCampaign camp;
    camp.id = QStringLiteral("c1");
    camp.name = QStringLiteral("Test Campaign");
    camp.accountLinkUrl = QStringLiteral("https://example.com/link");
    GqlTimeBasedDrop d1;
    d1.id = QStringLiteral("d1");
    d1.requiredMinutesWatched = 60;
    d1.currentMinutesWatched = 10;
    camp.drops.append(d1);

    GqlDropCampaign inv;
    inv.id = QStringLiteral("c1");
    inv.name = QStringLiteral("Test Campaign");
    GqlTimeBasedDrop invD1;
    invD1.id = QStringLiteral("d1");
    invD1.requiredMinutesWatched = 60;
    invD1.currentMinutesWatched = 45;
    invD1.dropInstanceId = QStringLiteral("inst-1");
    inv.drops.append(invD1);

    GqlDropCampaign invOnly;
    invOnly.id = QStringLiteral("c2");
    invOnly.name = QStringLiteral("Inventory Only");

    const auto merged =
        drops::detail::mergeCampaignsAndInventory({camp}, {inv, invOnly});
    ASSERT_EQ(merged.size(), 2);
    EXPECT_EQ(merged.at(0).accountLinkUrl,
              QStringLiteral("https://example.com/link"));
    ASSERT_EQ(merged.at(0).drops.size(), 1);
    EXPECT_EQ(merged.at(0).drops.at(0).currentMinutesWatched, 45);
    EXPECT_EQ(merged.at(0).drops.at(0).dropInstanceId,
              QStringLiteral("inst-1"));
    EXPECT_EQ(merged.at(1).id, QStringLiteral("c2"));
}

TEST(TwitchChannelPoints, ClaimAvailablePubSub)
{
    WatchDropsMockApplication app(Settings::instance());
    getSettings()->autoClaimChannelPoints = false;

    TwitchChannel channel(QStringLiteral("testchannel"));
    TwitchChannelTestAccess::setRoomId(channel, QStringLiteral("999"));

    QJsonObject claim;
    claim.insert("id", QStringLiteral("claim-abc"));
    claim.insert("channel_id", QStringLiteral("999"));

    QJsonObject data;
    data.insert("claim", claim);

    QJsonObject payload;
    payload.insert("type", QStringLiteral("claim-available"));
    payload.insert("topic", QStringLiteral("community-points-user-v1.123"));
    payload.insert("data", data);

    channel.handleUserPointsUpdate(payload);
    EXPECT_TRUE(channel.hasAvailableBonusClaim());
    EXPECT_EQ(channel.availableBonusClaimId(), QStringLiteral("claim-abc"));

    QJsonObject pointGain;
    pointGain.insert("reason_code", QStringLiteral("CLAIM"));
    QJsonObject balance;
    balance.insert("channel_id", QStringLiteral("999"));
    balance.insert("balance", 150);
    QJsonObject earnedData;
    earnedData.insert("point_gain", pointGain);
    earnedData.insert("balance", balance);
    QJsonObject earnedPayload;
    earnedPayload.insert("type", QStringLiteral("points-earned"));
    earnedPayload.insert("topic", QStringLiteral("community-points-user-v1.123"));
    earnedPayload.insert("data", earnedData);

    channel.handleUserPointsUpdate(earnedPayload);
    EXPECT_FALSE(channel.hasAvailableBonusClaim());
    EXPECT_EQ(channel.channelPointBalance(), 150);
}

}  // namespace chatterino

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);

    QApplication app(argc, argv);
    QLocale::setDefault(QLocale("en_GB"));
    chatterino::initResources();
    chatterino::NetworkManager::init();

    chatterino::Modes modes;
    chatterino::Args args;
    QTemporaryDir settingsDir;
    settingsDir.setAutoRemove(false);
    chatterino::Settings settings(modes, args, settingsDir.path());

    QTimer::singleShot(0, [&]() {
        const auto res = RUN_ALL_TESTS();
        chatterino::NetworkManager::deinit();
        settingsDir.remove();

        for (size_t i = 0; i < 32; ++i)
        {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }

        QApplication::exit(res);
    });

    return QApplication::exec();
}
