// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/linux/backends.hpp"
#include "../src/linux/app-picker.hpp"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QQuickWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>
#include <X11/Xlib.h>
#undef KeyPress
#undef KeyRelease

class FixtureWindow final : public QWidget {
    QString path, typed;
    void write(const QString &suffix, const QByteArray &bytes) {
        QFile file(path + suffix);
        if (file.open(QIODevice::WriteOnly)) file.write(bytes);
    }
public:
    explicit FixtureWindow(const QString &output) : path(output) { setFocusPolicy(Qt::StrongFocus); }
protected:
    void paintEvent(QPaintEvent *) override { QPainter(this).fillRect(rect(), QColor("#2288cc")); }
    void mousePressEvent(QMouseEvent *) override { write(".click", "clicked"); setFocus(); }
    void keyPressEvent(QKeyEvent *event) override { typed += event->text(); write({}, typed.toUtf8()); }
    void resizeEvent(QResizeEvent *) override {
        write(".size", QByteArray::number(width()) + "," + QByteArray::number(height()));
    }
};

class BackendTests : public QObject {
    Q_OBJECT
private slots:
    void launcherResolution() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString desktop = directory.filePath("org.example.Test.desktop");
        QFile file(desktop);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[Desktop Entry]\nType=Application\nName=Test App\nExec=\"/opt/Test App/bin/app\" --label %c %U %% %i\nIcon=test-icon\nStartupWMClass=fixture-app\n[Desktop Action Other]\nExec=wrong-app\n");
        file.close();
        const auto launch = resolveAppLaunch("fixture-app", {}, {directory.path()}, directory.filePath("profiles"));
        QCOMPARE(launch.name, QString("Test App"));
        QCOMPARE(launch.arguments, QStringList({"/opt/Test App/bin/app", "--label", "Test App", "%", "--icon", "test-icon"}));
        QCOMPARE(splitLaunchCommand(launch.command()), launch.arguments);
        AppLaunch quoted;
        quoted.arguments = {"/path with spaces/app", "a\"b", "", "x\\y"};
        QCOMPARE(splitLaunchCommand(quoted.command()), quoted.arguments);
        const auto fallback = resolveAppLaunch("Unknown", "/usr/bin/true", {}, {});
        QCOMPARE(fallback.arguments, QStringList({"/usr/bin/true"}));
        QVERIFY(resolveAppLaunch("Unknown", "/not/an/executable", {}, {}).arguments.isEmpty());
    }
    void browserProfilesAndOverrides() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDir().mkpath(directory.filePath("user"));
        QDir().mkpath(directory.filePath("system"));
        QFile firefox(directory.filePath("system/firefox.desktop"));
        QVERIFY(firefox.open(QIODevice::WriteOnly));
        firefox.write("[Desktop Entry]\nType=Application\nName=Firefox\nExec=firefox --profile /old/profile %U\n"); firefox.close();
        const QStringList dirs{directory.filePath("user"), directory.filePath("system")};
        const auto launch = resolveAppLaunch("firefox", {}, dirs, directory.filePath("profiles"));
        QVERIFY(launch.arguments.contains("--new-instance"));
        QVERIFY(!launch.arguments.contains("/old/profile"));
        QCOMPARE(launch.profileDirectory, directory.filePath("profiles/firefox"));
        QCOMPARE(splitLaunchCommand(launch.command()), launch.arguments);
        QFile hidden(directory.filePath("user/firefox.desktop"));
        QVERIFY(hidden.open(QIODevice::WriteOnly));
        hidden.write("[Desktop Entry]\nHidden=true\n"); hidden.close();
        QVERIFY(resolveAppLaunch("firefox", {}, dirs, {}).arguments.isEmpty());
        QFile brave(directory.filePath("system/brave.desktop"));
        QVERIFY(brave.open(QIODevice::WriteOnly));
        brave.write("[Desktop Entry]\nType=Application\nName=Brave\nExec=brave-browser-stable --user-data-dir=/old --ozone-platform=x11 %U\n"); brave.close();
        const auto chromium = resolveAppLaunch("brave", {}, dirs, directory.filePath("profiles"));
        QVERIFY(chromium.arguments.contains("--ozone-platform=wayland"));
        QVERIFY(!chromium.arguments.contains("--user-data-dir=/old"));
        QVERIFY(chromium.arguments.contains("--user-data-dir=" + chromium.profileDirectory));
    }
    void pickerSelectsFocusedApp() {
        if (!AppPicker::available()) QSKIP("Needs an active Hyprland session");
        // Start a real native client with a distinct PID. The test process itself
        // is ignored by the picker, exactly as OBS is ignored in production.
        QTemporaryDir directory;
        QProcess client;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "wayland");
        client.setProcessEnvironment(env);
        client.start(QCoreApplication::applicationFilePath(), {"--fixture", directory.filePath("picked")});
        QVERIFY(client.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(directory.filePath("picked.size")), 5000);
        QProcess focus;
        focus.start("hyprctl", {"dispatch", "focuswindow", "pid:" + QString::number(client.processId())});
        QVERIFY(focus.waitForFinished(2000));
        auto fixtureFocused = [&] {
            QProcess active;
            active.start("hyprctl", {"-j", "activewindow"});
            if (!active.waitForFinished(1000)) return false;
            return QJsonDocument::fromJson(active.readAllStandardOutput()).object().value("pid").toInteger() == client.processId();
        };
        QTRY_VERIFY_WITH_TIMEOUT(fixtureFocused(), 5000);
        AppPicker picker(directory.filePath("profiles"));
        QSignalSpy selected(&picker, &AppPicker::selected);
        QSignalSpy failed(&picker, &AppPicker::failed);
        picker.start();
        QTRY_VERIFY_WITH_TIMEOUT(!selected.isEmpty() || !failed.isEmpty(), 10000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(splitLaunchCommand(selected.first().first().toString()).first(), QCoreApplication::applicationFilePath());
        QVERIFY(!picker.isPicking());
        client.terminate();
        QVERIFY(client.waitForFinished());
        picker.start(); picker.cancel();
        QVERIFY(!picker.isPicking());
    }
    void invalidCommand() {
        NestedWaylandDock dock;
        QString error;
        QVERIFY(!dock.start({}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!dock.isRunning());
        QVERIFY(!dock.start("/definitely-not-an-installed-app", &error));
        QVERIFY(!dock.isRunning());
    }
    void waylandLifecycle() {
        if (QStandardPaths::findExecutable("weston-terminal").isEmpty())
            QSKIP("Install weston-terminal to exercise a real Wayland client");
        NestedWaylandDock dock;
        dock.resize(640, 480); dock.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dock));
        QSignalSpy surfaces(&dock, &NestedWaylandDock::surfaceCountChanged);
        QString error;
        QVERIFY2(dock.start("weston-terminal", &error), qPrintable(error));
        const QString socket = qEnvironmentVariable("XDG_RUNTIME_DIR") + "/" + dock.socketName();
        QVERIFY(QFile::exists(socket));
        QTRY_COMPARE_WITH_TIMEOUT(dock.surfaceCount(), 1, 10000);
        QVERIFY(!surfaces.isEmpty());
        dock.resize(800, 600);
        QTest::qWait(300);
        QVERIFY(dock.isRunning());
        dock.stop();
        QCOMPARE(dock.surfaceCount(), 0);
        QVERIFY(!QFile::exists(socket));
        QVERIFY(!dock.isRunning());
        // A second launch catches stale socket, compositor and process ownership.
        QVERIFY2(dock.start("weston-terminal", &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(dock.surfaceCount(), 1, 10000);
        dock.stop();
        QVERIFY2(dock.start("/usr/bin/true", &error), qPrintable(error));
        QTest::qWait(50);
        dock.stop();
    }
    void waylandClientCloses() {
        QTemporaryDir directory;
        NestedWaylandDock dock;
        dock.resize(640, 480); dock.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dock));
        QString error;
        const QString command = "\"" + QCoreApplication::applicationFilePath() + "\" --fixture \"" + directory.filePath("closed") + "\" --auto-close";
        QVERIFY2(dock.start(command, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(dock.surfaceCount(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(dock.surfaceCount(), 0, 5000);
        dock.stop();
    }
    void waylandInputAndIsolation() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString output = directory.filePath("typed.txt");
        NestedWaylandDock first, second;
        first.resize(640, 480); first.show();
        second.resize(420, 300); second.show();
        QVERIFY(QTest::qWaitForWindowExposed(&first));
        QVERIFY(QTest::qWaitForWindowExposed(&second));
        QString error;
        const QString fixture = "\"" + QCoreApplication::applicationFilePath() + "\" --fixture \"" + output + "\"";
        QVERIFY2(first.start(fixture, &error), qPrintable(error));
        QVERIFY2(second.start(fixture + ".second", &error), qPrintable(error));
        QVERIFY(first.socketName() != second.socketName());
        QTRY_COMPARE_WITH_TIMEOUT(first.surfaceCount(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(second.surfaceCount(), 1, 10000);
        auto *view = first.findChild<QQuickWidget *>();
        QVERIFY(view);
        auto painted = [&] {
            const QImage image = view->grabFramebuffer();
            return !image.isNull() && image.pixelColor(image.width()/2, image.height()/2) == QColor("#2288cc");
        };
        QTRY_VERIFY_WITH_TIMEOUT(painted(), 10000);
        first.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&first));
        view->setFocus(Qt::MouseFocusReason);
        const QPointF center(view->width()/2, view->height()/2);
        QMouseEvent click(QEvent::MouseButtonPress, center, view->mapToGlobal(center.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent releaseClick(QEvent::MouseButtonRelease, center, view->mapToGlobal(center.toPoint()),
                                 Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view, &click);
        QCoreApplication::sendEvent(view, &releaseClick);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output + ".click"), 5000);
        for (const auto &key : QList<QPair<int, int>>{{Qt::Key_A, 38}, {Qt::Key_B, 56}}) {
            const QString text = QString(QChar(key.first)).toLower();
            QKeyEvent press(QEvent::KeyPress, key.first, Qt::NoModifier, key.second, key.first, 0, text);
            QKeyEvent release(QEvent::KeyRelease, key.first, Qt::NoModifier, key.second, key.first, 0, text);
            QCoreApplication::sendEvent(view, &press);
            QCoreApplication::sendEvent(view, &release);
        }
        auto text = [&] { QFile file(output); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
        QTRY_COMPARE_WITH_TIMEOUT(text(), QByteArray("ab"), 5000);
        const QImage frame = view->grabFramebuffer();
        QVERIFY(!frame.isNull());
        QCOMPARE(frame.pixelColor(frame.width()/2, frame.height()/2), QColor("#2288cc"));
        auto clientSize = [&] { QFile file(output + ".size"); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
        const QByteArray oldSize = clientSize();
        first.resize(900, 700);
        QTRY_VERIFY_WITH_TIMEOUT(clientSize() != oldSize, 5000);
        const QString firstSocket = qEnvironmentVariable("XDG_RUNTIME_DIR") + "/" + first.socketName();
        first.stop();
        QVERIFY(!QFile::exists(firstSocket));
        QCOMPARE(second.surfaceCount(), 1);
        QVERIFY(second.isRunning());
        second.stop();
    }
    void xorgLifecycle() {
        if (QGuiApplication::platformName() != "xcb") QSKIP("Run with QT_QPA_PLATFORM=xcb");
        Display *display = XOpenDisplay(nullptr);
        QVERIFY(display);
        Window root = DefaultRootWindow(display);
        Window window = XCreateSimpleWindow(display, root, 10, 20, 320, 240, 0, 0, 0);
        XStoreName(display, window, "window-dock integration fixture");
        XMapWindow(display, window); XSync(display, False);
        XorgDock dock; dock.resize(640, 480); dock.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dock));
        QString error;
        QVERIFY2(dock.attach(window, &error), qPrintable(error));
        Window parent, returnedRoot, *children = nullptr; unsigned int count;
        QVERIFY(XQueryTree(display, window, &returnedRoot, &parent, &children, &count));
        if (children) XFree(children);
        QCOMPARE(parent, Window(dock.winId()));
        dock.resize(800, 600); QTest::qWait(50);
        XWindowAttributes attr; XGetWindowAttributes(display, window, &attr);
        QCOMPARE(attr.width, int(dock.width() * dock.devicePixelRatioF()));
        dock.detach(); XSync(display, False);
        QVERIFY(XQueryTree(display, window, &returnedRoot, &parent, &children, &count));
        if (children) XFree(children);
        QCOMPARE(parent, root);
        XGetWindowAttributes(display, window, &attr);
        QCOMPARE(attr.width, 320); QCOMPARE(attr.height, 240);
        QVERIFY2(dock.attach(window, &error), qPrintable(error));
        XDestroyWindow(display, window); XSync(display, False);
        QTRY_VERIFY_WITH_TIMEOUT(!dock.attached(), 2000);
        dock.detach();
        QVERIFY(!dock.attach(window, &error));
        XCloseDisplay(display);
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (app.arguments().value(1) == "--fixture") {
        const QString path = app.arguments().value(2);
        FixtureWindow window(path);
        window.resize(400, 300);
        window.show();
        if (app.arguments().contains("--auto-close"))
            QTimer::singleShot(300, &window, &QWidget::close);
        return app.exec();
    }
    BackendTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "backends.moc"
