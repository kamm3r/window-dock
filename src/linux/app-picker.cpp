// SPDX-License-Identifier: GPL-3.0-or-later
#include "app-picker.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <unistd.h>

namespace {
QString unescape(const QString &value) {
    QString out;
    for (qsizetype i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const QChar next = value[i+1];
            if (next == 's') { out += ' '; ++i; continue; }
            if (next == 'n') { out += '\n'; ++i; continue; }
            if (next == 't') { out += '\t'; ++i; continue; }
            if (next == 'r') { out += '\r'; ++i; continue; }
            if (next == '\\') { out += '\\'; ++i; continue; }
        }
        out += value[i];
    }
    return out;
}
QMap<QString, QString> readDesktop(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QMap<QString, QString> values;
    bool inEntry = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith('[')) { inEntry = line == "[Desktop Entry]"; continue; }
        if (!inEntry || line.startsWith('#')) continue;
        const qsizetype equal = line.indexOf('=');
        if (equal > 0) values.insert(line.left(equal), unescape(line.mid(equal+1)));
    }
    return values;
}
QStringList desktopArguments(const QMap<QString, QString> &entry, const QString &path) {
    QStringList tokens;
    QString token;
    bool quoted = false, started = false;
    const QString exec = entry.value("Exec");
    for (qsizetype i = 0; i < exec.size(); ++i) {
        const QChar ch = exec[i];
        if (ch == '\\' && i + 1 < exec.size()) { token += exec[++i]; started = true; }
        else if (ch == '"') { quoted = !quoted; started = true; }
        else if (ch.isSpace() && !quoted) {
            if (started) tokens.append(token);
            token.clear(); started = false;
        } else { token += ch; started = true; }
    }
    if (quoted) return {};
    if (started) tokens.append(token);
    QStringList args;
    for (const QString &part : tokens) {
        // No file/URL is being opened. Flatpak's paired file-forwarding markers
        // must also be omitted when their argument lists are empty.
        if (part == "%f" || part == "%F" || part == "%u" || part == "%U" ||
            part == "%d" || part == "%D" || part == "%n" || part == "%N" ||
            part == "%v" || part == "%m" || part == "@@" || part == "@@u") continue;
        if (part == "%i") {
            if (!entry.value("Icon").isEmpty()) args << "--icon" << entry.value("Icon");
            continue;
        }
        QString expanded;
        for (qsizetype i = 0; i < part.size(); ++i) {
            if (part[i] != '%') { expanded += part[i]; continue; }
            if (++i == part.size()) return {};
            if (part[i] == '%') expanded += '%';
            else if (part[i] == 'c') expanded += entry.value("Name");
            else if (part[i] == 'k') expanded += path;
            else return {}; // Unsupported/malformed field code: do not guess.
        }
        args.append(expanded);
    }
    return args;
}
QString executableName(const QStringList &args) {
    if (args.isEmpty()) return {};
    if (QFileInfo(args[0]).fileName() != "env") return QFileInfo(args[0]).fileName();
    for (qsizetype i = 1; i < args.size(); ++i)
        if (!args[i].contains('=') && !args[i].startsWith('-')) return QFileInfo(args[i]).fileName();
    return {};
}
void removeOption(QStringList &args, const QString &option, bool value) {
    for (qsizetype i = 1; i < args.size();) {
        if (args[i] == option) {
            args.removeAt(i);
            if (value && i < args.size()) args.removeAt(i);
        } else if (args[i].startsWith(option + '=')) args.removeAt(i);
        else ++i;
    }
}
}
QStringList splitLaunchCommand(const QString &command) {
    QStringList arguments;
    QString token;
    bool quoted = false, started = false;
    for (qsizetype i = 0; i < command.size();) {
        if (command.mid(i, 3) == "\"\"\"") {
            token += '"'; started = true; i += 3;
        } else if (command[i] == '"') {
            quoted = !quoted; started = true; ++i;
        } else if (command[i].isSpace() && !quoted) {
            if (started) arguments.append(token);
            token.clear(); started = false; ++i;
        } else { token += command[i++]; started = true; }
    }
    if (quoted) return {};
    if (started) arguments.append(token);
    return arguments;
}
QString AppLaunch::command() const {
    QStringList quoted;
    for (QString argument : arguments) {
        // Use Qt-style triple quotes for a literal quote; preserve empty arguments.
        argument.replace('"', "\"\"\"");
        quoted.append('"' + argument + '"');
    }
    return quoted.join(' ');
}
AppLaunch resolveAppLaunch(const QString &appId, const QString &executable,
                           const QStringList &directories, const QString &profileRoot) {
    AppLaunch result;
    int bestScore = 0;
    QSet<QString> visited;
    const QString exeName = QFileInfo(executable).fileName();
    for (const QString &directory : directories) {
        QDirIterator files(directory, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            QString desktopId = QDir(directory).relativeFilePath(path);
            desktopId.replace('/', '-');
            if (visited.contains(desktopId)) continue;
            visited.insert(desktopId); // User entries, including Hidden=true, override system entries.
            const auto entry = readDesktop(path);
            if (entry.value("Hidden") == "true" || entry.value("Type") != "Application" ||
                entry.value("Terminal") == "true") continue;
            const QStringList args = desktopArguments(entry, path);
            if (args.isEmpty()) continue;
            int score = 0;
            if (!appId.isEmpty() && desktopId.chopped(8).compare(appId, Qt::CaseInsensitive) == 0) score = 100;
            else if (!appId.isEmpty() && entry.value("StartupWMClass").compare(appId, Qt::CaseInsensitive) == 0) score = 90;
            else if (!exeName.isEmpty() && executableName(args) == exeName) score = 40;
            if (score <= bestScore) continue;
            bestScore = score;
            result.arguments = args;
            result.name = entry.value("Name", appId);
        }
    }
    if (result.arguments.isEmpty()) {
        if (executable.isEmpty() || !QFileInfo(executable).isExecutable()) return {};
        result.arguments = {executable};
        result.name = appId.isEmpty() ? exeName : appId;
    }
    const QString program = executableName(result.arguments).toLower();
    const bool firefox = program == "firefox" || program == "firefox-bin" || program == "firefox-esr";
    const bool chromium = program == "chromium" || program == "chromium-browser" ||
        program.startsWith("google-chrome") || program.startsWith("brave-browser") || program.startsWith("microsoft-edge");
    if ((firefox || chromium) && !profileRoot.isEmpty()) {
        result.profileDirectory = QDir(profileRoot).filePath(program);
        if (firefox) {
            for (const QString &option : {QString("--profile"), QString("-profile"), QString("-P")})
                removeOption(result.arguments, option, true);
            removeOption(result.arguments, "--new-instance", false);
            result.arguments << "--new-instance" << "--profile" << result.profileDirectory;
        } else {
            for (const QString &option : {QString("--user-data-dir"), QString("--profile-directory"), QString("--ozone-platform")})
                removeOption(result.arguments, option, true);
            result.arguments << "--ozone-platform=wayland" << ("--user-data-dir=" + result.profileDirectory)
                             << "--no-first-run" << "--no-default-browser-check";
        }
        result.notice = "Uses a separate dock browser profile; existing tabs and logins are not copied.";
    } else {
        result.notice = "Some single-instance apps need extra options. You can edit the command before launching.";
    }
    return result;
}
AppPicker::AppPicker(const QString &root, QObject *parent) : QObject(parent), profileRoot(root) {
    poll.setInterval(300);
    deadline.setSingleShot(true);
    queryDeadline.setSingleShot(true);
    connect(&poll, &QTimer::timeout, this, &AppPicker::query);
    connect(&deadline, &QTimer::timeout, this, [this] { fail("Selection timed out. Click Pick app and focus another app within 30 seconds."); });
    connect(&queryDeadline, &QTimer::timeout, this, [this] { fail("Hyprland did not respond to the focused-window query."); });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (picking) fail("Cannot query Hyprland: " + process.errorString());
    });
    connect(&process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        queryDeadline.stop();
        if (!picking) return;
        if (exitCode != 0) { fail("Hyprland rejected the focused-window query."); return; }
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(process.readAllStandardOutput(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            fail("Hyprland returned invalid focused-window information."); return;
        }
        const auto window = document.object();
        const qint64 pid = window.value("pid").toInteger();
        QString appId = window.value("initialClass").toString();
        if (appId.isEmpty()) appId = window.value("class").toString();
        if (window.value("hidden").toBool() || window.value("mapped") == false) return;
        if (pid <= 0 || pid == QCoreApplication::applicationPid() ||
            appId.contains("obsproject", Qt::CaseInsensitive) ||
            QFileInfo("/proc/" + QString::number(pid)).ownerId() != getuid()) return;
        const QString executable = QFileInfo("/proc/" + QString::number(pid) + "/exe").symLinkTarget();
        const auto launch = resolveAppLaunch(appId, executable,
            QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation), profileRoot);
        if (launch.arguments.isEmpty()) { fail("Could not identify a launcher for the selected app. Enter its command manually."); return; }
        if (!launch.profileDirectory.isEmpty() && !QDir().mkpath(launch.profileDirectory)) {
            fail("Could not create the dock's separate browser profile directory."); return;
        }
        cancel();
        emit selected(launch.command(), launch.name, launch.notice);
    });
}
AppPicker::~AppPicker() {
    picking = false;
    process.disconnect(this);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
    }
}
bool AppPicker::available() {
    return !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") &&
           !QStandardPaths::findExecutable("hyprctl").isEmpty();
}
bool AppPicker::isPicking() const { return picking; }
void AppPicker::start() {
    if (picking) return;
    if (!available()) { emit failed("Pick app currently supports Hyprland. Enter a launch command on other desktops."); return; }
    picking = true;
    emit pickingChanged(true);
    deadline.start(30000);
    poll.start();
    query();
}
void AppPicker::cancel() {
    picking = false;
    poll.stop(); deadline.stop(); queryDeadline.stop();
    if (process.state() != QProcess::NotRunning) process.kill();
    emit pickingChanged(false);
}
void AppPicker::fail(const QString &message) { cancel(); emit failed(message); }
void AppPicker::query() {
    if (!picking || process.state() != QProcess::NotRunning) return;
    process.start(QStandardPaths::findExecutable("hyprctl"), {"-j", "activewindow"});
    queryDeadline.start(2000);
}
