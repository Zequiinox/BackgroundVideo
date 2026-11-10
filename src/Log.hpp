#pragma once

#include <android/log.h>
#include <filesystem>

namespace bgv::log {

// Logs to logcat and, once a file is set, to <configDir>/log.txt (truncated on each start).
void setFile(const std::filesystem::path& path);
void write(int priority, const char* format, ...) __attribute__((format(printf, 2, 3)));

}

#define LOGI(...) ::bgv::log::write(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) ::bgv::log::write(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) ::bgv::log::write(ANDROID_LOG_ERROR, __VA_ARGS__)
