// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/StreamLink.hpp"

#include "Application.hpp"
#include "common/LinkParser.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "providers/twitch/WatchHeartbeatService.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/QualityPopup.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <QDir>
#include <QErrorMessage>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QStringBuilder>

#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#endif

namespace {

using namespace chatterino;

QString getStreamlinkPath()
{
    if (getSettings()->streamlinkUseCustomPath)
    {
        const QString path = getSettings()->streamlinkPath;
        return path.trimmed() % "/" % STREAMLINK_BINARY_NAME;
    }

    return STREAMLINK_BINARY_NAME.toString();
}

void showStreamlinkNotFoundError()
{
    static auto *msg = new QErrorMessage;
    msg->setWindowTitle("Chatterino - streamlink not found");

    if (getSettings()->streamlinkUseCustomPath)
    {
        msg->showMessage("Unable to find Streamlink executable\nMake sure "
                         "your custom path is pointing to the DIRECTORY "
                         "where the streamlink executable is located");
    }
    else
    {
        msg->showMessage(
            "Unable to find Streamlink executable.\nIf you have Streamlink "
            "installed, you might need to enable the custom path option");
    }
}

QProcess *createStreamlinkProcess()
{
    auto *p = new QProcess;

    const auto path = getStreamlinkPath();

    if (Version::instance().isFlatpak())
    {
        p->setProgram("flatpak-spawn");
        p->setArguments({"--host", path});
    }
    else
    {
        p->setProgram(path);
    }

    QObject::connect(p, &QProcess::errorOccurred, [=](auto err) {
        if (err == QProcess::FailedToStart)
        {
            showStreamlinkNotFoundError();
        }
        else
        {
            qCWarning(chatterinoStreamlink) << "Error occurred" << err;
        }

        p->deleteLater();
    });

    QObject::connect(
        p,
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
            &QProcess::finished),
        [=](int , QProcess::ExitStatus ) {
            p->deleteLater();
        });

    return p;
}

QString preferredStreamlinkQualityString(QStringList &args)
{
    const auto preferredQuality = getSettings()->preferredQuality.getEnum();
    QString quality;
    QString exclude;

    if (preferredQuality == StreamLinkPreferredQuality::High)
    {
        exclude = ">720p30";
        quality = "high,best";
    }
    else if (preferredQuality == StreamLinkPreferredQuality::Medium)
    {
        exclude = ">540p30";
        quality = "medium,best";
    }
    else if (preferredQuality == StreamLinkPreferredQuality::Low)
    {
        exclude = ">360p30";
        quality = "low,best";
    }
    else if (preferredQuality == StreamLinkPreferredQuality::AudioOnly)
    {
        quality = "audio,audio_only";
    }
    else
    {
        quality = "best";
    }

    if (!exclude.isEmpty())
    {
        args << "--stream-sorting-excludes" << exclude;
    }

    return quality;
}

}

namespace chatterino {

QString resolveMpvExecutable()
{
    const auto custom = getSettings()->streamlinkMpvPath.getValue().trimmed();
    if (!custom.isEmpty())
    {
        return custom;
    }

    if (const auto fromPath =
            QStandardPaths::findExecutable(QStringLiteral("mpv"));
        !fromPath.isEmpty())
    {
        return fromPath;
    }

#ifdef Q_OS_WIN
    if (const auto fromPathExe =
            QStandardPaths::findExecutable(QStringLiteral("mpv.exe"));
        !fromPathExe.isEmpty())
    {
        return fromPathExe;
    }

    static const QStringList candidates{
        QStringLiteral("C:/Program Files/MPV Player/mpv.exe"),
        QStringLiteral("C:/Program Files/MPV Player/mpv.com"),
        QStringLiteral("C:/Program Files (x86)/MPV Player/mpv.exe"),
        QStringLiteral("C:/Program Files/mpv/mpv.exe"),
    };
    for (const auto &candidate : candidates)
    {
        if (QFileInfo::exists(candidate))
        {
            return candidate;
        }
    }
#endif

    return {};
}

QStringList buildStreamlinkPlayerArgs(const QStringList &existingArgs,
                                      bool useMpv, const QString &mpvExecutable)
{
    const auto trimmedMpv = mpvExecutable.trimmed();
    if (!useMpv || trimmedMpv.isEmpty())
    {
        return {};
    }

    for (const auto &arg : existingArgs)
    {
        if (arg == QLatin1String("--player") || arg == QLatin1String("-p") ||
            arg.startsWith(QLatin1String("--player=")))
        {
            return {};
        }
    }

    return {QStringLiteral("--player"), trimmedMpv};
}

std::optional<IdleMpvSession> launchIdleMpvForChannel(
    const QString &channelLogin)
{
    const auto mpv = resolveMpvExecutable();
    if (mpv.isEmpty())
    {
        return std::nullopt;
    }

    const auto login = channelLogin.trimmed().toLower();
    const auto nonce = QRandomGenerator::global()->generate();
#ifdef Q_OS_WIN
    const QString ipcPath =
        QStringLiteral(R"(\\.\pipe\luckymoltorino-mpv-%1-%2)")
            .arg(login)
            .arg(nonce);
#else
    const QString ipcPath =
        QDir::tempPath() +
        QStringLiteral("/luckymoltorino-mpv-%1-%2.sock").arg(login).arg(nonce);
#endif

    const QStringList args{
        QStringLiteral("--idle=yes"),
        QStringLiteral("--force-window=yes"),
        QStringLiteral("--keep-open=yes"),
        QStringLiteral("--title=Waiting for %1...").arg(login),
        QStringLiteral("--input-ipc-server=%1").arg(ipcPath),
    };

    qint64 pid = 0;
    if (!QProcess::startDetached(mpv, args, QString(), &pid) || pid <= 0)
    {
        return std::nullopt;
    }

    return IdleMpvSession{.pid = pid, .ipcPipePath = ipcPath};
}

bool sendMpvIpcCommand(const QString &ipcPipePath, const QJsonArray &command)
{
    if (ipcPipePath.isEmpty())
    {
        return false;
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("command"), command);
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    line.append('\n');

#ifdef Q_OS_WIN
    HANDLE pipe = CreateFileW(reinterpret_cast<LPCWSTR>(ipcPipePath.utf16()),
                              GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, line.constData(), DWORD(line.size()),
                              &written, nullptr);
    CloseHandle(pipe);
    return ok != FALSE && written == DWORD(line.size());
#else
    QLocalSocket socket;
    socket.connectToServer(ipcPipePath);
    if (!socket.waitForConnected(1500))
    {
        return false;
    }
    socket.write(line);
    const bool flushed = socket.waitForBytesWritten(1500);
    socket.disconnectFromServer();
    return flushed;
#endif
}

void resolveStreamlinkStreamUrl(const QString &channelLogin,
                                std::function<void(QString)> onSuccess,
                                std::function<void(QString)> onError)
{
    auto *p = createStreamlinkProcess();
    const auto login = channelLogin.trimmed().toLower();
    QStringList args = p->arguments();
    args << QStringLiteral("--stream-url");

    QStringList extraArgs;
    const QString quality = preferredStreamlinkQualityString(extraArgs);
    args << extraArgs;

    const QString additionalOptions =
        getSettings()->streamlinkOpts.getValue();
    args << QProcess::splitCommand(additionalOptions);
    args << QStringLiteral("twitch.tv/%1").arg(login) << quality;
    args.removeAll(QString());

    QObject::connect(
        p,
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
            &QProcess::finished),
        [p, onSuccess = std::move(onSuccess),
         onError = std::move(onError)](int exitCode, QProcess::ExitStatus) {
            const auto stdoutText =
                QString::fromUtf8(p->readAllStandardOutput());
            const auto stderrText =
                QString::fromUtf8(p->readAllStandardError()).trimmed();

            if (exitCode == 0)
            {
                const auto lines = stdoutText.split('\n', Qt::SkipEmptyParts);
                for (auto it = lines.crbegin(); it != lines.crend(); ++it)
                {
                    const auto candidate = it->trimmed();
                    if (candidate.startsWith(QLatin1String("http://")) ||
                        candidate.startsWith(QLatin1String("https://")))
                    {
                        onSuccess(candidate);
                        return;
                    }
                }
            }

            onError(stderrText.isEmpty()
                        ? QStringLiteral("Streamlink exited with code %1")
                              .arg(exitCode)
                        : stderrText);
        });

    p->setArguments(args);
    p->start();
}

void getStreamQualities(const QString &channelURL,
                        std::function<void(QStringList)> cb)
{
    auto *p = createStreamlinkProcess();

    QObject::connect(
        p,
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
            &QProcess::finished),
        [=](int exitCode, QProcess::ExitStatus ) {
            if (exitCode != 0)
            {
                qCWarning(chatterinoStreamlink) << "Got error code" << exitCode;

            }
            QString lastLine = QString(p->readAllStandardOutput());
            lastLine = lastLine.trimmed().split('\n').last().trimmed();
            if (lastLine.startsWith("Available streams: "))
            {
                QStringList options;
                QStringList split =
                    lastLine.right(lastLine.length() - 19).split(", ");

                for (auto i = split.length() - 1; i >= 0; i--)
                {
                    QString option = split.at(i);
                    if (option == "best)")
                    {

                        option = split.at(--i);

                        options << option.left(option.length() - 7);
                    }
                    else if (option.endsWith(" (worst)"))
                    {
                        options << option.left(option.length() - 8);
                    }
                    else if (option.endsWith(" (best)"))
                    {
                        options << option.left(option.length() - 7);
                    }
                    else
                    {
                        options << option;
                    }
                }

                cb(options);
            }
        });

    p->setArguments(p->arguments() +
                    QStringList{channelURL, "--default-stream=KKona"});

    p->start();
}

void openStreamlink(const QString &channelURL, const QString &quality,
                    QStringList extraArguments)
{
    const auto proc = std::unique_ptr<QProcess>(createStreamlinkProcess());
    auto arguments = proc->arguments() << std::move(extraArguments);

    const QString additionalOptions = getSettings()->streamlinkOpts.getValue();
    const auto extraUserArgs = QProcess::splitCommand(additionalOptions);

    QStringList allExisting = arguments;
    allExisting << extraUserArgs;
    arguments << buildStreamlinkPlayerArgs(
        allExisting, getSettings()->streamlinkUseMpv, resolveMpvExecutable());
    arguments << extraUserArgs << channelURL << quality;

    arguments.removeAll(QString());

    proc->setArguments(arguments);
    qint64 pid = 0;
    bool res = proc->startDetached(&pid);

    if (!res)
    {
        showStreamlinkNotFoundError();
        return;
    }

    if (pid > 0 && getSettings()->trackStreamlinkWatchTime &&
        getSettings()->enableWatchHeartbeat)
    {
        if (const auto login =
                watchheartbeat::detail::extractTwitchLoginFromUrl(channelURL))
        {
            if (auto *app = tryGetApp(); app && app->getWatchHeartbeat())
            {
                app->getWatchHeartbeat()->trackExternalProcess(
                    pid, *login, QStringLiteral("streamlink:%1").arg(pid));
            }
        }
    }
}

void openStreamlinkForChannel(const QString &channel, QStringView prefixURL)
{
    static const QString INFO_TEMPLATE("Opening %1 in Streamlink ...");

    SplitContainer *currentPage = getApp()
                                      ->getWindows()
                                      ->getLastSelectedWindow()
                                      ->getNotebook()
                                      .getSelectedPage();
    if (currentPage != nullptr)
    {
        auto *currentSplit = currentPage->getSelectedSplit();
        if (currentSplit != nullptr)
        {
            currentSplit->getChannel()->addSystemMessage(
                INFO_TEMPLATE.arg(channel));
        }
    }

    const auto target = channel.trimmed();
    const auto parsed = linkparser::parse(target);
    const bool isCompleteUrl =
        parsed && !parsed->hasPrefix(target) && !parsed->hasSuffix(target);
    const QString channelURL = isCompleteUrl ? target : prefixURL % target;

    auto preferredQuality = getSettings()->preferredQuality.getEnum();

    if (preferredQuality == StreamLinkPreferredQuality::Choose)
    {
        getStreamQualities(channelURL, [=](QStringList qualityOptions) {
            QualityPopup::showDialog(channelURL, qualityOptions);
        });

        return;
    }

    QStringList args;
    const QString quality = preferredStreamlinkQualityString(args);

    openStreamlink(channelURL, quality, args);
}

}
