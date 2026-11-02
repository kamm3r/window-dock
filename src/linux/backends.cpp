// SPDX-License-Identifier: GPL-3.0-or-later
#include "backends.hpp"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QQuickWidget>
#include <QQuickItem>
#include <QResizeEvent>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QtWaylandCompositor/QWaylandQuickCompositor>
#include <QtWaylandCompositor/QWaylandQuickOutput>
#include <QtWaylandCompositor/QWaylandQuickShellSurfaceItem>
#include <QtWaylandCompositor/QWaylandXdgShell>
#include <QtWaylandCompositor/QWaylandSeat>
#include <QtWaylandCompositor/QWaylandSurface>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <unistd.h>

namespace {
// All Xlib access here is on the GUI thread, using our own connection.
int xError = 0;
int recordXError(Display *, XErrorEvent *event) { xError = event->error_code; return 0; }
class XErrors {
    Display *display;
    XErrorHandler previous;
public:
    explicit XErrors(Display *d) : display(d) {
        XSync(display, False);
        xError = 0;
        previous = XSetErrorHandler(recordXError);
    }
    bool ok() { XSync(display, False); return xError == 0; }
    ~XErrors() { XSync(display, False); XSetErrorHandler(previous); }
};
QString readWindowTitle(Display *d, Window w) {
    Atom type; int format; unsigned long count, remaining; unsigned char *bytes = nullptr;
    const Atom name = XInternAtom(d, "_NET_WM_NAME", False);
    const Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
    QString result;
    if (XGetWindowProperty(d, w, name, 0, 4096, False, utf8, &type, &format,
                           &count, &remaining, &bytes) == Success && bytes) {
        if (format == 8) result = QString::fromUtf8(reinterpret_cast<char *>(bytes), int(count));
        XFree(bytes);
    }
    if (result.isEmpty()) {
        char *nameText = nullptr;
        if (XFetchName(d, w, &nameText) && nameText) {
            result = QString::fromLocal8Bit(nameText);
            XFree(nameText);
        }
    }
    return result;
}
}
struct XorgDock::State {
    Display *display = nullptr;
    Window window = 0, parent = 0;
    int x = 0, y = 0, rootX = 0, rootY = 0, width = 0, height = 0;
    bool mapped = false;
};
XorgDock::XorgDock(QWidget *parent) : QWidget(parent), state(std::make_unique<State>()) {
    setAttribute(Qt::WA_NativeWindow);
    setMinimumSize(160, 100);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!state->window) return;
        XErrors errors(state->display);
        XWindowAttributes attributes;
        if (!XGetWindowAttributes(state->display, state->window, &attributes) || !errors.ok())
            state->window = 0;
    });
    timer->start(500);
}
XorgDock::~XorgDock() { detach(); if (state->display) XCloseDisplay(state->display); }
bool XorgDock::attached() const { return state->window != 0; }
QList<DesktopWindow> XorgDock::windows() {
    QList<DesktopWindow> result;
    Display *d = XOpenDisplay(nullptr);
    if (!d) return result;
    {
        XErrors errors(d);
        Window root = DefaultRootWindow(d);
        Atom type; int format; unsigned long count = 0, remaining; unsigned char *bytes = nullptr;
        XGetWindowProperty(d, root, XInternAtom(d, "_NET_CLIENT_LIST", False), 0, 65536,
                           False, XA_WINDOW, &type, &format, &count, &remaining, &bytes);
        QList<Window> candidates;
        if (bytes && format == 32 && type == XA_WINDOW) {
            auto *items = reinterpret_cast<Window *>(bytes);
            for (unsigned long i = 0; i < count; ++i) candidates.append(items[i]);
        }
        if (bytes) XFree(bytes);
        if (candidates.isEmpty()) {
            Window returnedRoot, parent, *children = nullptr; unsigned int childCount = 0;
            if (XQueryTree(d, root, &returnedRoot, &parent, &children, &childCount))
                for (unsigned int i = 0; i < childCount; ++i) candidates.append(children[i]);
            if (children) XFree(children);
        }
        for (Window w : candidates) {
            XWindowAttributes attr;
            if (!XGetWindowAttributes(d, w, &attr) || attr.map_state != IsViewable) continue;
            unsigned char *pid = nullptr; unsigned long n;
            XGetWindowProperty(d, w, XInternAtom(d, "_NET_WM_PID", False), 0, 1, False,
                               XA_CARDINAL, &type, &format, &n, &remaining, &pid);
            bool own = pid && format == 32 && n && *reinterpret_cast<unsigned long *>(pid) == static_cast<unsigned long>(getpid());
            if (pid) XFree(pid);
            const QString title = readWindowTitle(d, w);
            if (!own && !title.isEmpty()) result.append({w, title});
        }
    }
    XCloseDisplay(d);
    return result;
}
bool XorgDock::attach(unsigned long w, QString *error) {
    detach();
    if (QGuiApplication::platformName() != "xcb") {
        *error = "Existing Xorg windows require OBS running with QT_QPA_PLATFORM=xcb. Use a launched Wayland app on native Wayland.";
        return false;
    }
    if (!state->display) state->display = XOpenDisplay(nullptr);
    if (!state->display) { *error = "Cannot connect to the X server."; return false; }
    Display *d = state->display;
    XErrors errors(d);
    Window root, parent, *children = nullptr; unsigned int count;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(d, w, &attr) ||
        !XQueryTree(d, w, &root, &parent, &children, &count)) {
        if (children) XFree(children);
        *error = "The selected window no longer exists."; return false;
    }
    if (children) XFree(children);
    state->parent = parent;
    state->x = attr.x; state->y = attr.y;
    Window child;
    XTranslateCoordinates(d, w, root, 0, 0, &state->rootX, &state->rootY, &child);
    state->width = attr.width; state->height = attr.height;
    state->mapped = attr.map_state != IsUnmapped;
    XAddToSaveSet(d, w);
    XReparentWindow(d, w, Window(winId()), 0, 0);
    XResizeWindow(d, w, qMax(1, int(width() * devicePixelRatioF())),
                           qMax(1, int(height() * devicePixelRatioF())));
    XMapWindow(d, w);
    if (!errors.ok()) {
        XReparentWindow(d, w, parent, attr.x, attr.y);
        XRemoveFromSaveSet(d, w);
        *error = "The X server rejected embedding this window."; return false;
    }
    state->window = w;
    return true;
}
void XorgDock::detach() {
    if (!state->window || !state->display) return;
    Display *d = state->display;
    XErrors errors(d);
    XWindowAttributes attr;
    if (XGetWindowAttributes(d, state->window, &attr)) {
        Window parent = state->parent;
        int x = state->x, y = state->y;
        if (!XGetWindowAttributes(d, parent, &attr)) {
            parent = DefaultRootWindow(d);
            x = state->rootX; y = state->rootY;
        }
        XReparentWindow(d, state->window, parent, x, y);
        XResizeWindow(d, state->window, state->width, state->height);
        if (state->mapped) XMapWindow(d, state->window);
        XRemoveFromSaveSet(d, state->window);
    }
    state->window = 0;
}
void XorgDock::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (!state->window) return;
    XErrors errors(state->display);
    XResizeWindow(state->display, state->window,
                  qMax(1, int(width() * devicePixelRatioF())), qMax(1, int(height() * devicePixelRatioF())));
}

struct NestedWaylandDock::State {
    QWaylandQuickCompositor *compositor = nullptr;
    QQuickWindow *window = nullptr;
    QWidget *container = nullptr;
    QProcess *process = nullptr;
    QList<QPointer<QWaylandXdgToplevel>> toplevels;
    QString socket;
};
NestedWaylandDock::NestedWaylandDock(QWidget *parent) : QWidget(parent), state(std::make_unique<State>()) {
    setMinimumSize(160, 100);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
}
NestedWaylandDock::~NestedWaylandDock() { stop(); }
QString NestedWaylandDock::socketName() const { return state->socket; }
int NestedWaylandDock::surfaceCount() const { return state->toplevels.size(); }
bool NestedWaylandDock::isRunning() const { return state->compositor != nullptr; }
bool NestedWaylandDock::start(const QString &command, QString *error) {
    if (isRunning()) { *error = "Stop the current app before launching another."; return false; }
    QStringList args = QProcess::splitCommand(command);
    if (args.isEmpty()) { *error = "Enter an executable and optional arguments."; return false; }
    if (qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR")) {
        *error = "XDG_RUNTIME_DIR must be set for the nested Wayland socket."; return false;
    }
    state->socket = "obs-window-dock-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto *quickWidget = new QQuickWidget(this);
    quickWidget->setClearColor(QColor("#202124"));
    quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quickWidget->setContent(QUrl(), nullptr, new QQuickItem);
    state->container = quickWidget;
    state->window = quickWidget->quickWindow();
    state->window->setObjectName("window-dock-compositor-" + state->socket);
    state->container->setFocusPolicy(Qt::StrongFocus);
    layout()->addWidget(state->container);
    state->compositor = new QWaylandQuickCompositor(this);
    state->compositor->setSocketName(state->socket.toUtf8());
    auto *output = new QWaylandQuickOutput(state->compositor, state->window);
    output->setParent(state->compositor);
    output->setSizeFollowsWindow(true);
    auto *shell = new QWaylandXdgShell(state->compositor);
    // Extension containers do not imply QObject ownership.
    shell->setParent(state->compositor);
    connect(shell, &QWaylandXdgShell::toplevelCreated, this,
            [this](QWaylandXdgToplevel *toplevel, QWaylandXdgSurface *surface) {
        state->toplevels.append(toplevel);
        auto *item = new QWaylandQuickShellSurfaceItem(state->window->contentItem());
        item->setShellSurface(surface);
        item->setFocusOnClick(true);
        item->setAutoCreatePopupItems(true);
        auto configure = [this, toplevel] {
            toplevel->sendMaximized(state->window->size().expandedTo(QSize(1, 1)));
        };
        connect(state->window, &QWindow::widthChanged, toplevel, configure);
        connect(state->window, &QWindow::heightChanged, toplevel, configure);
        const QPointer<QWaylandXdgToplevel> tracked = toplevel;
        connect(surface->surface(), &QWaylandSurface::surfaceDestroyed, item, [this, item, tracked] {
            state->toplevels.removeAll(tracked);
            item->deleteLater();
            emit surfaceCountChanged(surfaceCount());
            if (!surfaceCount()) emit statusChanged("App windows closed. Stop to release the compositor.");
        });
        configure();
        state->compositor->defaultSeat()->setKeyboardFocus(surface->surface());
        item->forceActiveFocus();
        emit surfaceCountChanged(surfaceCount());
        emit statusChanged("App connected — click the dock to interact.");
    });
    state->compositor->create();
    // OBS can continuously render without the GUI event dispatcher going idle.
    // Bound dispatch/flush latency rather than relying only on aboutToBlock.
    auto *dispatch = new QTimer(state->compositor);
    connect(dispatch, &QTimer::timeout, state->compositor, &QWaylandCompositor::processWaylandEvents);
    dispatch->start(16);
    if (!state->compositor->isCreated()) {
        *error = "Could not create the nested Wayland compositor.";
        stop(); return false;
    }
    state->container->show();
    state->process = new QProcess(this);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("WAYLAND_DISPLAY", state->socket);
    env.insert("QT_QPA_PLATFORM", "wayland");
    env.insert("GDK_BACKEND", "wayland");
    env.insert("SDL_VIDEODRIVER", "wayland");
    env.remove("DISPLAY");
    env.remove("LD_PRELOAD"); // Do not propagate OBS's plugin-runtime override to launched apps.
    state->process->setProcessEnvironment(env);
    state->process->setProcessChannelMode(QProcess::ForwardedChannels);
    connect(state->process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit statusChanged(state->process->errorString());
    });
    connect(state->process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        emit statusChanged(QString("Launcher exited (%1). Stop releases any remaining app windows.").arg(code));
    });
    const QString program = args.takeFirst();
    state->process->start(program, args);
    if (!state->process->waitForStarted(3000)) {
        *error = state->process->errorString();
        stop(); return false;
    }
    return true;
}
void NestedWaylandDock::stop() {
    if (state->process) {
        state->process->disconnect(this);
        state->process->terminate();
        if (!state->process->waitForFinished(1000)) {
            state->process->kill();
            state->process->waitForFinished(1000);
        }
        delete state->process;
        state->process = nullptr;
    }
    // Tear down scene graph views before the surfaces/compositor they reference.
    delete state->container; // Owns the QQuickWindow and synchronizes its render thread.
    state->container = nullptr;
    state->window = nullptr;
    if (state->compositor) {
        state->compositor->disconnect(this);
        delete state->compositor;
        state->compositor = nullptr;
    }
    state->toplevels.clear();
    state->socket.clear();
    emit surfaceCountChanged(0);
}
