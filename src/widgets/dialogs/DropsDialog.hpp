#pragma once

#include "providers/twitch/api/TwitchGql.hpp"
#include "widgets/DraggablePopup.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QString>
#include <QVBoxLayout>
#include <QVector>

namespace chatterino {

class Label;
class SvgButton;

class DropsDialog final : public DraggablePopup
{
    Q_OBJECT

public:
    explicit DropsDialog(QWidget *parent = nullptr);
    ~DropsDialog() override;

    static void showDialog(QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;

private:
    void refreshHeader();
    void refreshStyle();
    void rebuildContent();
    void clearContent();
    void triggerRefresh();

    enum class TabFilter {
        InProgress,
        AllActive,
    };

    TabFilter activeTab_ = TabFilter::InProgress;

    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QLabel *headerSubtitleLabel_{};
    QPushButton *refreshButton_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};

    QWidget *watchBarWidget_{};
    QLabel *watchStatusLabel_{};
    QPushButton *stopWatchButton_{};

    QPushButton *inProgressTabButton_{};
    QPushButton *allActiveTabButton_{};

    QScrollArea *scrollArea_{};
    QWidget *contentWidget_{};
    QVBoxLayout *contentLayout_{};

    pajlada::Signals::SignalHolder signalHolder_;

    static QPointer<DropsDialog> activeDialog_;
};

}  // namespace chatterino
