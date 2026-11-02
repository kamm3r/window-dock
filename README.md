# Window Dock — Linux fork

Experimental Linux port of [OVERKLOC/window-dock](https://github.com/OVERKLOC/window-dock).
The original Windows implementation is retained. Linux uses separate backends:

- **Wayland:** launch a native Wayland app inside an interactive nested compositor
  hosted by an OBS dock. Mouse, keyboard and client resizing are forwarded.
  Each dock has its own socket and compositor. This also runs on an Xorg desktop.
- **Xorg:** select and embed an existing X11 application window. Releasing the
  window restores its parent and size. The X save-set also protects embedded
  windows if the plugin's X connection terminates unexpectedly.

This is a prototype, not a production-ready release. The backend integration
suite is tested on Hyprland and an isolated Xvfb display with Qt 6.11.2 and OBS
32.2.2 headers. Full OBS workflows and arbitrary third-party apps still require
hands-on testing. The original Windows build has not been retested.

## Using the Linux plugin

1. Install the built `window-dock.so`, then restart OBS.
2. Open **Tools → Window Dock — Linux**.
3. Enter a dock name and choose **Launch Wayland app (interactive)**.
4. Click **Add dock**. In the dock, enter an executable and arguments, for example
   `weston-terminal`, and click **Launch app**.
5. Click the app area to use its mouse and keyboard. **Stop app** closes the
   compositor and disconnects its clients. Removing a dock stops its app, too.

Commands are parsed into an executable and arguments, without a shell: pipes,
redirection, shell variables and `~` expansion are not interpreted. Use absolute
paths where necessary. Apps must support native Wayland; there is no nested
Xwayland server. Single-instance apps may forward their launch to an existing
process on the main desktop. Use their documented separate-instance/profile
options instead. For example, use a dedicated Firefox profile with `--no-remote`.

App command lines and dock definitions are saved under OBS's plugin configuration
as `window-dock/linux-docks.json`. Apps are **never automatically launched** on
startup; click Launch again after restarting OBS. Existing Xorg windows must be
selected again after restarting. Dock placement restoration has not yet been
verified inside OBS.

For **Embed existing Xorg window**, OBS itself must run with the `xcb` Qt backend:

```sh
QT_QPA_PLATFORM=xcb obs
```

On a Wayland desktop that runs OBS via Xwayland. It embeds X11 windows only, not
existing native Wayland windows. Native Wayland OBS can use launched Wayland apps.

## Limitations

- Existing native Wayland windows cannot be imported from the desktop compositor.
- Multiple top-level windows from one launched app share and overlap in the same
  dock; there is no internal window switcher yet. Simple single-window apps work
  best. Qt handles transient popup surfaces, but app-specific menus need testing.
- No desktop-to-nested clipboard bridge, drag/drop bridge or nested Xwayland.
- The nested compositor is **not a security sandbox**. Launched apps run as your
  user with access to your files and session services.
- The Wayland backend uses GPLv3 Qt Wayland Compositor. The new Linux sources are
  GPL-3.0-or-later; upstream Windows files retain their original license.
- This machine falls back to shared-memory client buffers. Hardware buffer
  integration depends on the host EGL/driver setup; heavy apps can be expensive.
- OBS or desktop global shortcuts may intercept keys intended for the app.
- Stopping a dock disconnects all its Wayland clients and terminates its direct
  launch process. Apps that daemonize may leave unrelated helper processes alive.

## Building on Linux

Requires CMake >= 3.16, a C++17 compiler, libobs and OBS frontend development files,
Xlib development files, and Qt >= 6.5 development modules: Widgets, Quick,
QuickWidgets and WaylandCompositor. For tests, also install Qt Test and
`weston-terminal` (usually part of Weston).

On Fedora/Nobara, the core development packages are:

```sh
sudo dnf install cmake ninja-build gcc-c++ obs-studio-devel libX11-devel \
  qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtwayland-devel
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib64
cmake --build build-linux
```

Use an out-of-source build. This fork removes the machine-specific root
`CMakeCache.txt` committed upstream.

To build an RPM:

```sh
cpack --config build-linux/CPackConfig.cmake -G RPM -B release
sudo dnf install ./release/*.rpm
```

Or install for the current user, without an RPM (choose one installation method):

```sh
install -Dm755 build-linux/window-dock.so \
  ~/.config/obs-studio/plugins/window-dock/bin/64bit/window-dock.so
```

## Backend integration tests

```sh
cmake -S . -B build-linux -DWINDOW_DOCK_BUILD_TESTS=ON
cmake --build build-linux
QT_QPA_PLATFORM=wayland ./build-linux/window-dock-tests
QT_QPA_PLATFORM=xcb ./build-linux/window-dock-tests
```

The tests exercise a real Weston client and a separate Qt client process:
failed launch cleanup, repeated launch/stop, socket cleanup, rendered pixels,
mouse clicks, typed text, resize delivery, and two independent nested docks.
Xorg tests verify reparenting, resizing, original geometry restoration and client
window destruction. Xorg tests skip on native Wayland; the Weston lifecycle test
skips if Weston is not installed. An Xvfb server can be used for isolated X11 tests.
