// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace chatterino {

class Exception : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

#ifdef Q_OS_WIN
constexpr inline QStringView STREAMLINK_BINARY_NAME = u"streamlink.exe";
#else
constexpr inline QStringView STREAMLINK_BINARY_NAME = u"streamlink";
#endif

struct IdleMpvSession {
    qint64 pid = 0;
    QString ipcPipePath;
};

QString resolveMpvExecutable();
QStringList buildStreamlinkPlayerArgs(const QStringList &existingArgs,
                                      bool useMpv, const QString &mpvExecutable);

std::optional<IdleMpvSession> launchIdleMpvForChannel(
    const QString &channelLogin);
bool sendMpvIpcCommand(const QString &ipcPipePath, const QJsonArray &command);
void resolveStreamlinkStreamUrl(const QString &channelLogin,
                                std::function<void(QString)> onSuccess,
                                std::function<void(QString)> onError);

void openStreamlink(const QString &channelURL, const QString &quality,
                    QStringList extraArguments = QStringList());

void openStreamlinkForChannel(const QString &channel,
                              QStringView prefixURL = u"twitch.tv/");

}
