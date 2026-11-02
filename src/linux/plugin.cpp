// SPDX-License-Identifier: GPL-3.0-or-later
#include "backends.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QFormLayout>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QDir>
#include <QUuid>
#include <QVBoxLayout>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("OVERKLOC; Linux port contributors")
MODULE_EXPORT const char *obs_module_description(void) { return "Interactive Xorg and nested Wayland application docks"; }

namespace {
struct Entry { QString id, name, command; bool wayland; QPointer<QWidget> content; };
QList<Entry> entries;
bool exiting = false;
QPointer<QDialog> manager;
QString configPath() {
    char *path = obs_module_config_path("linux-docks.json");
    QString result = QString::fromUtf8(path);
    bfree(path);
    return result;
}
void save() {
    if (exiting) return;
    QJsonArray data;
    for (const auto &entry : entries)
        data.append(QJsonObject{{"id", entry.id}, {"name", entry.name},
                                {"command", entry.command}, {"wayland", entry.wayland}});
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(data).toJson()) < 0 || !file.commit())
        blog(LOG_WARNING, "[window-dock] Cannot save dock configuration");
}
void addDock(const QString &id, const QString &name, bool wayland, const QString &command = {}) {
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);
    auto *status = new QLabel;
    status->setWordWrap(true);
    if (wayland) {
        auto *input = new QLineEdit(command);
        input->setPlaceholderText("Executable and arguments, e.g. weston-terminal");
        input->setToolTip("Use a separate app instance. Shell operators and pipes are not interpreted.");
        auto *buttons = new QHBoxLayout;
        auto *launch = new QPushButton("Launch app");
        auto *stop = new QPushButton("Stop app");
        stop->setEnabled(false);
        buttons->addWidget(launch); buttons->addWidget(stop);
        layout->addWidget(input); layout->addLayout(buttons);
        auto *dock = new NestedWaylandDock;
        layout->addWidget(dock, 1);
        QObject::connect(dock, &NestedWaylandDock::statusChanged, status, &QLabel::setText);
        QObject::connect(launch, &QPushButton::clicked, content, [=] {
            QString error;
            status->setText("Starting app…");
            if (!dock->start(input->text(), &error)) { status->setText(error); return; }
            launch->setEnabled(false); input->setEnabled(false); stop->setEnabled(true);
            for (auto &entry : entries) if (entry.id == id) entry.command = input->text();
            save();
        });
        QObject::connect(stop, &QPushButton::clicked, content, [=] {
            dock->stop();
            launch->setEnabled(true); input->setEnabled(true); stop->setEnabled(false);
            status->setText("Stopped. Closing the compositor disconnects apps launched inside it.");
        });
        status->setText("Launch a native Wayland app here. Existing desktop windows cannot be imported.");
    } else {
        auto *windows = new QComboBox;
        auto *buttons = new QHBoxLayout;
        auto *refresh = new QPushButton("Refresh");
        auto *attach = new QPushButton("Embed");
        auto *detach = new QPushButton("Release window");
        buttons->addWidget(refresh); buttons->addWidget(attach); buttons->addWidget(detach);
        layout->addWidget(windows); layout->addLayout(buttons);
        auto *dock = new XorgDock;
        layout->addWidget(dock, 1);
        auto populate = [=] {
            windows->clear();
            for (const auto &window : XorgDock::windows())
                windows->addItem(window.title, QVariant::fromValue<qulonglong>(window.id));
        };
        QObject::connect(refresh, &QPushButton::clicked, content, populate);
        QObject::connect(attach, &QPushButton::clicked, content, [=] {
            QString error;
            if (!windows->currentData().isValid()) { status->setText("Select a window first."); return; }
            status->setText(dock->attach(windows->currentData().toULongLong(), &error)
                            ? "Window embedded. Release restores it to the desktop." : error);
        });
        QObject::connect(detach, &QPushButton::clicked, content, [=] {
            dock->detach(); status->setText("Window released."); populate();
        });
        const bool xcb = QGuiApplication::platformName() == "xcb";
        attach->setEnabled(xcb); refresh->setEnabled(xcb);
        status->setText(xcb ? "Select an existing Xorg application window."
                            : "Requires OBS running with QT_QPA_PLATFORM=xcb. Native Wayland: use Launch Wayland app.");
        if (xcb) populate();
    }
    layout->addWidget(status);
    if (!obs_frontend_add_dock_by_id(id.toUtf8(), name.toUtf8(), content)) {
        delete content;
        blog(LOG_WARNING, "[window-dock] Could not create dock %s", id.toUtf8().constData());
        return;
    }
    entries.append({id, name, command, wayland, content});
}
void showManager() {
    if (manager) { manager->show(); manager->raise(); manager->activateWindow(); return; }
    auto *dialog = new QDialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
    manager = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Window Dock — Linux");
    auto *layout = new QVBoxLayout(dialog);
    auto *name = new QLineEdit("App Dock");
    auto *mode = new QComboBox;
    mode->addItem("Launch Wayland app (interactive)", true);
    mode->addItem("Embed existing Xorg window", false);
    layout->addWidget(new QLabel("Dock name")); layout->addWidget(name); layout->addWidget(mode);
    auto *add = new QPushButton("Add dock"); layout->addWidget(add);
    auto *existing = new QComboBox;
    auto populate = [=] {
        existing->clear();
        for (const auto &entry : entries) existing->addItem(entry.name, entry.id);
    };
    populate();
    layout->addWidget(new QLabel("Remove a dock (stops its launched app or releases its Xorg window)"));
    layout->addWidget(existing);
    auto *remove = new QPushButton("Remove dock"); layout->addWidget(remove);
    QObject::connect(add, &QPushButton::clicked, dialog, [=] {
        if (name->text().trimmed().isEmpty()) return;
        addDock("window-dock-" + QUuid::createUuid().toString(QUuid::WithoutBraces),
                name->text().trimmed(), mode->currentData().toBool());
        save(); populate();
    });
    QObject::connect(remove, &QPushButton::clicked, dialog, [=] {
        const QString id = existing->currentData().toString();
        for (qsizetype i = 0; i < entries.size(); ++i) {
            if (entries[i].id != id) continue;
            // Destroy backend resources before OBS removes the dock wrapper.
            delete entries[i].content.data();
            obs_frontend_remove_dock(id.toUtf8());
            entries.removeAt(i); break;
        }
        save(); populate();
    });
    dialog->resize(460, 260);
    dialog->show();
}
void event(enum obs_frontend_event type, void *) {
    if (type == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        QFile file(configPath());
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto data = QJsonDocument::fromJson(file.readAll()).array();
        QSet<QString> restored;
        for (const auto &value : data) {
            const auto entry = value.toObject();
            const QString id = entry["id"].toString();
            const QString name = entry["name"].toString();
            if (!id.startsWith("window-dock-") || name.isEmpty() || restored.contains(id)) continue;
            restored.insert(id);
            addDock(id, name, entry["wayland"].toBool(), entry["command"].toString());
        }
    } else if (type == OBS_FRONTEND_EVENT_EXIT) {
        save(); exiting = true;
        for (const auto &entry : entries) delete entry.content.data();
        entries.clear();
        delete manager.data();
    }
}
}
bool obs_module_load() {
    obs_frontend_add_tools_menu_item("Window Dock — Linux", [](void *) { showManager(); }, nullptr);
    obs_frontend_add_event_callback(event, nullptr);
    blog(LOG_INFO, "[window-dock] Linux backends loaded (Xorg embedding, nested Wayland)");
    return true;
}
void obs_module_unload() {
    obs_frontend_remove_event_callback(event, nullptr);
}
