// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QPointer>
#include <QProcess>
#include <QWidget>
#include <memory>

struct DesktopWindow {
    unsigned long id;
    QString title;
};

class XorgDock final : public QWidget {
    Q_OBJECT
public:
    explicit XorgDock(QWidget *parent = nullptr);
    ~XorgDock() override;
    static QList<DesktopWindow> windows();
    bool attach(unsigned long window, QString *error);
    void detach();
    bool attached() const;
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    struct State;
    std::unique_ptr<State> state;
};

class NestedWaylandDock final : public QWidget {
    Q_OBJECT
public:
    explicit NestedWaylandDock(QWidget *parent = nullptr);
    ~NestedWaylandDock() override;
    bool start(const QString &command, QString *error);
    void stop();
    QString socketName() const;
    int surfaceCount() const;
    bool isRunning() const;
signals:
    void statusChanged(const QString &message);
    void surfaceCountChanged(int count);
private:
    struct State;
    std::unique_ptr<State> state;
};
