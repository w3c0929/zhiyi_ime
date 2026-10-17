// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#ifndef CXXIME_SERVER_LAUNCHER_H_
#define CXXIME_SERVER_LAUNCHER_H_

#include <string>

namespace cxxime {

// Held by the running zhiyu-server for its sign-in session; a second server exits at once.
constexpr wchar_t kServerInstanceMutex[] = L"Local\\ZhiyuIME.Server";

// Starts the installed zhiyu-server.exe when the input method is switched to and the server is
// not running (startup.autostart off, or the server was exited from the taskbar menu). Skipped
// while the installer runs, and in sandboxed (AppContainer) or elevated programs: a server
// started there could not serve the other programs. Returns whether a server was started.
bool start_server_on_demand();

// The version directory that is installed *now* — HKLM's InstallLocation, which the installer
// keeps on the active version (no trailing slash). "" when nothing is installed (a source build)
// or the key cannot be read. A running server uses this when the directory it was itself started
// from is gone: an update retires that directory (its data/ included) while the old server keeps
// serving, and without it every settings save would be rejected (measured 2026-10-17).
std::string active_install_location();

} // namespace cxxime

#endif // CXXIME_SERVER_LAUNCHER_H_
