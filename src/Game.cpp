#include "Game.hpp"
#include "Hooks.hpp"
#include "Signatures.hpp"
#include "Log.hpp"
#include <atomic>


namespace bgv::game {
namespace {

using UpdateFn = void* (*)(void*, bool);
using LocalPlayerFn = void* (*)(void*);

UpdateFn gUpdateOriginal = nullptr;
LocalPlayerFn gGetLocalPlayer = nullptr;
hooks::Handle gUpdateHook = nullptr;
std::atomic_bool gInstalled{false};
std::atomic_bool gKnown{false};
std::atomic_bool gInMenu{false};

void* updateDetour(void* clientInstance, bool value) {
    void* result = gUpdateOriginal ? gUpdateOriginal(clientInstance, value) : nullptr;
    if (clientInstance && gGetLocalPlayer) {
        const bool menu = gGetLocalPlayer(clientInstance) == nullptr;
        if (gInMenu.exchange(menu, std::memory_order_acq_rel) != menu || !gKnown.load(std::memory_order_relaxed)) {
            LOGI("state: %s", menu ? "menu (no local player)" : "in world");
        }
        gKnown.store(true, std::memory_order_release);
    }
    return result;
}

}

bool install() {
    if (gInstalled.load(std::memory_order_acquire)) return true;
    if (!signatures::resolveAll("libminecraftpe.so")) {
        LOGE("signatures not found - this Minecraft build is not supported, menu detection disabled");
        return false;
    }
    gGetLocalPlayer = reinterpret_cast<LocalPlayerFn>(signatures::resolve(signatures::Id::ClientInstanceGetLocalPlayer));
    gUpdateHook = hooks::install(reinterpret_cast<void*>(signatures::resolve(signatures::Id::ClientInstanceUpdate)),
        reinterpret_cast<void*>(updateDetour), reinterpret_cast<void**>(&gUpdateOriginal));
    if (!gUpdateHook) {
        LOGE("failed to hook ClientInstance::update");
        return false;
    }
    gInstalled.store(true, std::memory_order_release);
    LOGI("game hooks installed");
    return true;
}

void uninstall() {
    if (gUpdateHook) hooks::remove(gUpdateHook);
    gUpdateHook = nullptr;
    gInstalled.store(false, std::memory_order_release);
    gKnown.store(false, std::memory_order_release);
    gInMenu.store(false, std::memory_order_release);
}

bool installed() { return gInstalled.load(std::memory_order_acquire); }
bool known() { return gKnown.load(std::memory_order_acquire); }
bool inMenu() { return gInMenu.load(std::memory_order_acquire); }

}
