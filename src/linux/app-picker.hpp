// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QStringList>

QStringList splitLaunchCommand(const QString &command);

struct AppLaunch {
    QStringList arguments;
    QString name;
    QString notice;
    QString profileDirectory;
    QString command() const;
};
AppLaunch resolveAppLaunch(const QString &appId, const QString &executable,
                           const QStringList &applicationDirectories, const QString &profileRoot);

class AppPicker final : public QObject {
    Q_OBJECT
public:
    explicit AppPicker(const QString &profileRoot, QObject *parent = nullptr);
    ~AppPicker() override;
    static bool available();
    bool isPicking() const;
    void start();
    void cancel();
signals:
    void selected(const QString &command, const QString &name, const QString &notice);
    void failed(const QString &message);
    void pickingChanged(bool picking);
private:
    void query();
    void fail(const QString &message);
    QProcess process;
    QTimer poll, deadline, queryDeadline;
    QString profileRoot;
    bool picking = false;
};
