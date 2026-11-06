// KAGE VR Stream - minimal thread-safe logger.
// Desktop: stderr + optional file. Android: logcat (tag "KageVR").
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace kage::log {

enum class Level { Debug = 0, Info, Warn, Error };

void setLevel(Level l);
Level level();
// Also append every line to this file (empty path disables). Returns false if it cannot be opened.
bool setFile(const std::string& path);
void write(Level l, std::string_view msg);

template <class... A>
void debug(std::format_string<A...> f, A&&... a) {
    if (level() <= Level::Debug) write(Level::Debug, std::format(f, std::forward<A>(a)...));
}
template <class... A>
void info(std::format_string<A...> f, A&&... a) {
    if (level() <= Level::Info) write(Level::Info, std::format(f, std::forward<A>(a)...));
}
template <class... A>
void warn(std::format_string<A...> f, A&&... a) {
    if (level() <= Level::Warn) write(Level::Warn, std::format(f, std::forward<A>(a)...));
}
template <class... A>
void error(std::format_string<A...> f, A&&... a) {
    write(Level::Error, std::format(f, std::forward<A>(a)...));
}

}  // namespace kage::log
