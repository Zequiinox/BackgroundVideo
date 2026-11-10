#include "Log.hpp"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace bgv::log {
namespace {

std::mutex gMutex;
std::filesystem::path gPath;
long gLines = 0;
const auto gStart = std::chrono::steady_clock::now();
constexpr long kMaxLines = 4000;

}

void setFile(const std::filesystem::path& path) {
    std::lock_guard lock(gMutex);
    gPath = path;
    gLines = 0;
    if (FILE* file = std::fopen(gPath.c_str(), "w")) std::fclose(file); // truncate; a failure here is harmless
}

void write(int priority, const char* format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    __android_log_print(priority, "BackgroundVideo", "%s", message);

    std::lock_guard lock(gMutex);
    if (gPath.empty() || gLines >= kMaxLines) return;
    if (FILE* file = std::fopen(gPath.c_str(), "a")) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gStart).count();
        std::fprintf(file, "[%7lld ms] %s\n", static_cast<long long>(ms), message);
        std::fclose(file);
        ++gLines;
    }
}

}
