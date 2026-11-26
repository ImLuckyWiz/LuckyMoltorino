#include "widgets/dialogs/DropsDialog.hpp"

#include "Application.hpp"
#include "providers/twitch/DropsService.hpp"
#include "providers/twitch/WatchHeartbeatService.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/helper/Line.hpp"

#include <QCursor>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

namespace {

constexpr QSize DEFAULT_DROPS_DIALOG_SIZE(460, 560);

bool campaignHasProgressOrClaimable(const GqlDropCampaign &campaign)
{
    for (const auto &drop : campaign.drops)
    {
        if (drop.currentMinutesWatched > 0 || drop.isClaimed ||
            !drop.dropInstanceId.isEmpty())
        {
            return true;
        }
    }
    return false;
}

}  // namespace

QPointer<DropsDialog> DropsDialog::activeDialog_;

DropsDialog::DropsDialog(QWidget *parent)
    : DraggablePopup(false, parent)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("DropsDialog");
    this->setWindowTitle("Twitch Drops");
    this->enableResize(getSettings()->dropsPopupSize, DEFAULT_DROPS_DIALOG_SIZE);

    auto *container = this->getLayoutContainer();
    container->setObjectName("DropsDialogRoot");
    this->mainLayout_ = new QVBoxLayout(container);
    this->mainLayout_->setSpacing(0);
    this->mainLayout_->setContentsMargins(0, 0, 0, 0);

    // Header
    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName("DropsDialogHeader");
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    headerLayout->setContentsMargins(10, 6, 8, 6);

    auto *headerTextLayout = new QVBoxLayout();
    headerTextLayout->setContentsMargins(0, 0, 0, 0);
    headerTextLayout->setSpacing(1);

    this->headerTitleLabel_ =
        new QLabel(QStringLiteral("Twitch Drops"), this->headerWidget_);
    this->headerTitleLabel_->setObjectName("DropsHeaderTitle");
    this->headerSubtitleLabel_ =
        new QLabel(QStringLiteral(""), this->headerWidget_);
    this->headerSubtitleLabel_->setObjectName("DropsHeaderSubtitle");
    headerTextLayout->addWidget(this->headerTitleLabel_);
    headerTextLayout->addWidget(this->headerSubtitleLabel_);
    headerLayout->addLayout(headerTextLayout);
    headerLayout->addStretch(1);

    this->refreshButton_ =
        new QPushButton(QStringLiteral("Refresh"), this->headerWidget_);
    this->refreshButton_->setObjectName("DropsUtilityButton");
    this->refreshButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->refreshButton_, &QPushButton::clicked, this,
                     [this] { this->triggerRefresh(); });
    headerLayout->addWidget(this->refreshButton_);

    this->pinButton_ = this->createPinButton();
    this->pinButton_->setToolTip(QStringLiteral("Pin Drops Window"));
    this->ensurePinned();
    headerLayout->addWidget(this->pinButton_);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize{3, 3});
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip(QStringLiteral("Close"));
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    headerLayout->addWidget(this->closeButton_);
    this->mainLayout_->addWidget(this->headerWidget_);

    auto *sep1 = new Line(false);
    sep1->setObjectName("DropsDialogSeparator");
    sep1->setFixedHeight(1);
    this->mainLayout_->addWidget(sep1);

    // Watch status bar
    this->watchBarWidget_ = new QWidget(container);
    this->watchBarWidget_->setObjectName("DropsWatchBar");
    auto *watchLayout = new QHBoxLayout(this->watchBarWidget_);
    watchLayout->setContentsMargins(10, 6, 10, 6);
    this->watchStatusLabel_ = new QLabel(this->watchBarWidget_);
    this->watchStatusLabel_->setObjectName("DropsWatchStatus");
    this->watchStatusLabel_->setWordWrap(true);
    watchLayout->addWidget(this->watchStatusLabel_, 1);

    this->stopWatchButton_ =
        new QPushButton(QStringLiteral("Stop"), this->watchBarWidget_);
    this->stopWatchButton_->setObjectName("DropsUtilityButton");
    this->stopWatchButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->stopWatchButton_, &QPushButton::clicked, this, [] {
        if (auto *app = tryGetApp(); app && app->getWatchHeartbeat())
        {
            app->getWatchHeartbeat()->stopWatching();
        }
    });
    watchLayout->addWidget(this->stopWatchButton_);
    this->mainLayout_->addWidget(this->watchBarWidget_);

    // Tab filter bar
    auto *tabsWidget = new QWidget(container);
    tabsWidget->setObjectName("DropsTabsBar");
    auto *tabsLayout = new QHBoxLayout(tabsWidget);
    tabsLayout->setContentsMargins(10, 4, 10, 6);
    tabsLayout->setSpacing(6);

    this->inProgressTabButton_ =
        new QPushButton(QStringLiteral("In Progress"), tabsWidget);
    this->inProgressTabButton_->setObjectName("DropsTabButton");
    this->inProgressTabButton_->setCheckable(true);
    this->inProgressTabButton_->setChecked(true);
    this->inProgressTabButton_->setCursor(Qt::PointingHandCursor);

    this->allActiveTabButton_ =
        new QPushButton(QStringLiteral("All Active Campaigns"), tabsWidget);
    this->allActiveTabButton_->setObjectName("DropsTabButton");
    this->allActiveTabButton_->setCheckable(true);
    this->allActiveTabButton_->setChecked(false);
    this->allActiveTabButton_->setCursor(Qt::PointingHandCursor);

    QObject::connect(this->inProgressTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->activeTab_ = TabFilter::InProgress;
                         this->inProgressTabButton_->setChecked(true);
                         this->allActiveTabButton_->setChecked(false);
                         this->rebuildContent();
                     });
    QObject::connect(this->allActiveTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->activeTab_ = TabFilter::AllActive;
                         this->inProgressTabButton_->setChecked(false);
                         this->allActiveTabButton_->setChecked(true);
                         this->rebuildContent();
                     });

    tabsLayout->addWidget(this->inProgressTabButton_);
    tabsLayout->addWidget(this->allActiveTabButton_);
    tabsLayout->addStretch(1);
    this->mainLayout_->addWidget(tabsWidget);

    auto *sep2 = new Line(false);
    sep2->setObjectName("DropsDialogSeparator");
    sep2->setFixedHeight(1);
    this->mainLayout_->addWidget(sep2);

    // Scrollable campaign list
    this->scrollArea_ = new QScrollArea(container);
    this->scrollArea_->setObjectName("DropsScrollArea");
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    this->contentWidget_ = new QWidget();
    this->contentWidget_->setObjectName("DropsDialogContent");
    this->contentLayout_ = new QVBoxLayout(this->contentWidget_);
    this->contentLayout_->setContentsMargins(10, 8, 10, 10);
    this->contentLayout_->setSpacing(8);
    this->scrollArea_->setWidget(this->contentWidget_);
    this->mainLayout_->addWidget(this->scrollArea_, 1);

    if (auto *app = tryGetApp())
    {
        if (app->getDrops() != nullptr)
        {
            this->signalHolder_.managedConnect(app->getDrops()->dropsChanged,
                                               [this] {
                                                   this->refreshHeader();
                                                   this->rebuildContent();
                                               });
        }
        if (app->getWatchHeartbeat() != nullptr)
        {
            this->signalHolder_.managedConnect(
                app->getWatchHeartbeat()->targetChanged, [this] {
                    this->refreshHeader();
                });
        }
    }

    this->refreshStyle();
    this->refreshHeader();
    this->rebuildContent();
}

DropsDialog::~DropsDialog() = default;

void DropsDialog::showDialog(QWidget *parent)
{
    if (!activeDialog_.isNull())
    {
        activeDialog_->raise();
        activeDialog_->activateWindow();
        activeDialog_->triggerRefresh();
        return;
    }

    auto *dialog = new DropsDialog(parent);
    activeDialog_ = dialog;

    QPoint center = QCursor::pos();
    if (parent != nullptr && parent->window() != nullptr)
    {
        center = parent->window()->geometry().center();
    }

    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(
        center - QPoint(size.width() / 2, size.height() / 2),
        widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
    dialog->triggerRefresh();
}

void DropsDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void DropsDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
}

void DropsDialog::triggerRefresh()
{
    if (auto *app = tryGetApp(); app && app->getDrops())
    {
        app->getDrops()->refresh(true);
    }
}

void DropsDialog::refreshHeader()
{
    auto *app = tryGetApp();
    if (app == nullptr)
    {
        return;
    }

    if (auto *hb = app->getWatchHeartbeat())
    {
        if (const auto target = hb->currentTarget())
        {
            this->watchStatusLabel_->setText(
                QStringLiteral("Watching: %1 (%2)")
                    .arg(target->login,
                         target->sourceTag.isEmpty() ? QStringLiteral("manual")
                                                     : target->sourceTag));
            this->stopWatchButton_->show();
        }
        else
        {
            this->watchStatusLabel_->setText(QStringLiteral(
                "Not watching any channel (use /watch or Open in Streamlink)"));
            this->stopWatchButton_->hide();
        }
    }

    if (auto *drops = app->getDrops())
    {
        this->refreshButton_->setEnabled(!drops->isLoading());
        if (drops->isLoading())
        {
            this->headerSubtitleLabel_->setText(QStringLiteral("Syncing..."));
        }
        else if (!drops->lastError().isEmpty())
        {
            this->headerSubtitleLabel_->setText(drops->lastError());
        }
        else if (drops->lastUpdatedAt().isValid())
        {
            this->headerSubtitleLabel_->setText(
                QStringLiteral("Updated %1")
                    .arg(drops->lastUpdatedAt().toLocalTime().toString(
                        QStringLiteral("HH:mm:ss"))));
        }
        else
        {
            this->headerSubtitleLabel_->setText(
                QStringLiteral("Ready"));
        }
    }
}

void DropsDialog::clearContent()
{
    while (QLayoutItem *item = this->contentLayout_->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->deleteLater();
        }
        delete item;
    }
}

void DropsDialog::rebuildContent()
{
    this->clearContent();

    auto *app = tryGetApp();
    if (app == nullptr || app->getDrops() == nullptr)
    {
        auto *lbl =
            new QLabel(QStringLiteral("Drops service unavailable."),
                       this->contentWidget_);
        this->contentLayout_->addWidget(lbl);
        this->contentLayout_->addStretch(1);
        return;
    }

    const auto allCampaigns = app->getDrops()->campaigns();
    QVector<GqlDropCampaign> visible;
    for (const auto &campaign : allCampaigns)
    {
        if (this->activeTab_ == TabFilter::InProgress)
        {
            if (campaignHasProgressOrClaimable(campaign))
            {
                visible.append(campaign);
            }
        }
        else
        {
            if (campaign.status.isEmpty() ||
                campaign.status.compare(QLatin1String("ACTIVE"),
                                        Qt::CaseInsensitive) == 0 ||
                campaignHasProgressOrClaimable(campaign))
            {
                visible.append(campaign);
            }
        }
    }

    if (visible.isEmpty())
    {
        const QString emptyText =
            app->getDrops()->isLoading()
                ? QStringLiteral("Loading Twitch Drops...")
                : (this->activeTab_ == TabFilter::InProgress
                       ? QStringLiteral("No Drops currently in progress. "
                                        "Check \"All Active Campaigns\" or "
                                        "watch a Drops-enabled stream.")
                       : QStringLiteral("No active Drop campaigns found."));
        auto *emptyLabel = new QLabel(emptyText, this->contentWidget_);
        emptyLabel->setObjectName("DropsEmptyLabel");
        emptyLabel->setWordWrap(true);
        this->contentLayout_->addWidget(emptyLabel);
        this->contentLayout_->addStretch(1);
        return;
    }

    for (const auto &campaign : visible)
    {
        auto *card = new QFrame(this->contentWidget_);
        card->setObjectName("DropsCampaignCard");
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(6);

        auto *topRow = new QHBoxLayout();
        topRow->setSpacing(6);

        const QString titleText =
            campaign.gameName.isEmpty()
                ? campaign.name
                : QStringLiteral("%1 — %2").arg(campaign.gameName,
                                                campaign.name);
        auto *titleLabel = new QLabel(titleText, card);
        titleLabel->setObjectName("DropsCampaignTitle");
        titleLabel->setWordWrap(true);
        topRow->addWidget(titleLabel, 1);

        if (!campaign.isAccountConnected && !campaign.accountLinkUrl.isEmpty())
        {
            auto *connectBtn =
                new QPushButton(QStringLiteral("Connect account"), card);
            connectBtn->setObjectName("DropsUtilityButton");
            connectBtn->setCursor(Qt::PointingHandCursor);
            const auto linkUrl = campaign.accountLinkUrl;
            QObject::connect(connectBtn, &QPushButton::clicked, this,
                             [linkUrl] {
                                 QDesktopServices::openUrl(QUrl(linkUrl));
                             });
            topRow->addWidget(connectBtn);
        }
        else if (campaign.isAccountConnected)
        {
            auto *linkedLbl = new QLabel(QStringLiteral("Linked"), card);
            linkedLbl->setObjectName("DropsLinkedLabel");
            topRow->addWidget(linkedLbl);
        }
        cardLayout->addLayout(topRow);

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
            auto *dropRow = new QHBoxLayout();
            dropRow->setSpacing(8);

            QString dropLabel = drop.name;
            if (dropLabel.isEmpty() && !drop.benefits.isEmpty())
            {
                dropLabel = drop.benefits.first().name;
            }
            if (dropLabel.isEmpty())
            {
                dropLabel = QStringLiteral("Drop Reward");
            }

            auto *leftCol = new QVBoxLayout();
            leftCol->setSpacing(2);

            auto *nameLbl = new QLabel(dropLabel, card);
            nameLbl->setObjectName("DropsDropName");
            nameLbl->setWordWrap(true);
            leftCol->addWidget(nameLbl);

            if (drop.requiredMinutesWatched > 0)
            {
                auto *bar = new QProgressBar(card);
                bar->setObjectName("DropsProgressBar");
                bar->setRange(0, drop.requiredMinutesWatched);
                bar->setValue(std::clamp(drop.currentMinutesWatched, 0,
                                         drop.requiredMinutesWatched));
                bar->setFormat(QStringLiteral("%1 / %2 min")
                                   .arg(bar->value())
                                   .arg(drop.requiredMinutesWatched));
                bar->setTextVisible(true);
                bar->setFixedHeight(16);
                leftCol->addWidget(bar);
            }

            dropRow->addLayout(leftCol, 1);

            if (drop.isClaimed)
            {
                auto *stateLbl = new QLabel(QStringLiteral("Claimed"), card);
                stateLbl->setObjectName("DropsClaimedLabel");
                dropRow->addWidget(stateLbl);
            }
            else if (drops::detail::isDropReadyToClaim(drop, claimedIds))
            {
                auto *claimBtn =
                    new QPushButton(QStringLiteral("Claim"), card);
                claimBtn->setObjectName("DropsClaimButton");
                claimBtn->setCursor(Qt::PointingHandCursor);
                const auto instanceId = drop.dropInstanceId;
                QObject::connect(claimBtn, &QPushButton::clicked, this,
                                 [instanceId, dropLabel] {
                                     if (auto *a = tryGetApp();
                                         a && a->getDrops())
                                     {
                                         a->getDrops()->claimDrop(
                                             instanceId, dropLabel, false);
                                     }
                                 });
                dropRow->addWidget(claimBtn);
            }
            else if (!drop.preconditionDropIds.isEmpty())
            {
                bool locked = false;
                for (const auto &preId : drop.preconditionDropIds)
                {
                    if (!claimedIds.contains(preId))
                    {
                        locked = true;
                        break;
                    }
                }
                if (locked)
                {
                    auto *lockedLbl =
                        new QLabel(QStringLiteral("Locked"), card);
                    lockedLbl->setObjectName("DropsMutedLabel");
                    dropRow->addWidget(lockedLbl);
                }
            }

            cardLayout->addLayout(dropRow);
        }

        this->contentLayout_->addWidget(card);
    }

    this->contentLayout_->addStretch(1);
}

void DropsDialog::refreshStyle()
{
    const auto *theme = this->theme;
    auto textColor = theme->window.text;
    auto mutedColor = textColor;
    mutedColor.setAlpha(165);

    const auto bg = theme->window.background.name();
    const auto text = textColor.name(QColor::HexArgb);
    const auto muted = mutedColor.name(QColor::HexArgb);
    const auto border = theme->splits.header.border.name();
    const auto cardBg = theme->splits.input.background.name();
    const auto accent = theme->splits.header.focusedBorder.name();

    this->setStyleSheet(QStringLiteral(R"(
        QWidget#DropsDialogRoot {
            background: %1;
            color: %2;
        }
        QLabel#DropsHeaderTitle, QLabel#DropsCampaignTitle, QLabel#DropsDropName {
            color: %2;
            font-weight: 600;
        }
        QLabel#DropsHeaderSubtitle, QLabel#DropsWatchStatus, QLabel#DropsEmptyLabel, QLabel#DropsMutedLabel {
            color: %3;
        }
        QLabel#DropsLinkedLabel, QLabel#DropsClaimedLabel {
            color: #4ade80;
            font-weight: 600;
        }
        QScrollArea#DropsScrollArea, QWidget#DropsDialogContent {
            background: transparent;
            border: 0;
        }
        QFrame#DropsCampaignCard {
            background: %5;
            border: 1px solid %4;
            border-radius: 6px;
        }
        QPushButton#DropsUtilityButton, QPushButton#DropsTabButton {
            background: %5;
            color: %2;
            border: 1px solid %4;
            border-radius: 4px;
            padding: 4px 8px;
        }
        QPushButton#DropsTabButton:checked {
            border-color: %6;
            font-weight: 600;
        }
        QPushButton#DropsClaimButton {
            background: #9147ff;
            color: #ffffff;
            border: 0;
            border-radius: 4px;
            padding: 4px 10px;
            font-weight: 600;
        }
        QProgressBar#DropsProgressBar {
            background: %1;
            color: %2;
            border: 1px solid %4;
            border-radius: 4px;
            text-align: center;
        }
        QProgressBar#DropsProgressBar::chunk {
            background: #9147ff;
            border-radius: 3px;
        }
    )")
                            .arg(bg, text, muted, border, cardBg, accent));
}

}  // namespace chatterino
