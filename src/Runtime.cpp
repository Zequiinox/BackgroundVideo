#include "Runtime.hpp"
#include "BackgroundVideo.hpp"
#include "Game.hpp"
#include "Hooks.hpp"
#include <android/log.h>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace bgv {
namespace {

std::atomic_bool gEnabled{false};
std::mutex gInstallMutex;
thread_local bool gInsideDlopen = false;
void* (*gDlopenOriginal)(const char*, int) = nullptr;
hooks::Handle gDlopenHook = nullptr;

void* dlopenDetour(const char* filename, int flags) {
    void* handle = gDlopenOriginal ? gDlopenOriginal(filename, flags) : nullptr;
    if (handle && filename && std::strstr(filename, "libminecraftpe.so") && !gInsideDlopen) {
        gInsideDlopen = true;
        Runtime::get().minecraftLoaded();
        gInsideDlopen = false;
    }
    return handle;
}

bool minecraftAlreadyLoaded() {
    void* handle = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return false;
    dlclose(handle);
    return true;
}

}

Runtime& Runtime::get() {
    static Runtime runtime;
    return runtime;
}

// Only act inside the process that actually runs Minecraft.
bool Runtime::launcherContext() const {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    char command[256]{};
    const auto size = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (size <= 0) return false;
    return std::strcmp(command, "org.levimc.launcher") == 0
        || std::strcmp(command, "org.levimc.launcher:minecraft") == 0
        || std::strcmp(command, "com.mojang.minecraftpe") == 0;
}

void Runtime::installGameHooks() {
    std::lock_guard lock(gInstallMutex);
    if (game::installed()) return;
    game::install();
}

void Runtime::minecraftLoaded() {
    if (gEnabled.load(std::memory_order_acquire)) installGameHooks();
}

bool Runtime::load(pl::mod::ModContext& context) {
    video::setup(context.javaVm(), context.resourceDir(), context.configDir());
    return true;
}

bool Runtime::enable(pl::mod::ModContext&) {
    if (!launcherContext()) return true;
    gEnabled.store(true, std::memory_order_release);
    video::setEnabled(true);
    video::installHooks();
    if (minecraftAlreadyLoaded()) {
        installGameHooks();
    } else if (!gDlopenHook) {
        void* symbol = hooks::symbolFromLibrary("libdl.so", "dlopen");
        if (symbol) gDlopenHook = hooks::install(symbol, reinterpret_cast<void*>(dlopenDetour), reinterpret_cast<void**>(&gDlopenOriginal));
    }
    return true;
}

bool Runtime::disable(pl::mod::ModContext&) {
    gEnabled.store(false, std::memory_order_release);
    video::setEnabled(false);
    return true;
}

bool Runtime::unload(pl::mod::ModContext&) {
    gEnabled.store(false, std::memory_order_release);
    video::setEnabled(false);
    video::removeHooks();
    game::uninstall();
    if (gDlopenHook) {
        hooks::remove(gDlopenHook);
        gDlopenHook = nullptr;
    }
    return true;
}

}
