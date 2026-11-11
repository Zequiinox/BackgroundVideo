#include "Signatures.hpp"
#include "Log.hpp"

#include <pl/memory/Vtable.hpp>

namespace bgv::signatures {
namespace {


constexpr const char* kClientInstance = "14ClientInstance";
constexpr std::size_t kSlotUpdate = 24;         // CI_update
constexpr std::size_t kSlotGetLocalPlayer = 31; // CI_getLocalPlayer

std::uintptr_t gAddresses[2]{};

}

bool resolveAll(const char* libraryName) {
    gAddresses[0] = pl::memory::resolveVtableFunction(kClientInstance, kSlotUpdate, libraryName);
    gAddresses[1] = pl::memory::resolveVtableFunction(kClientInstance, kSlotGetLocalPlayer, libraryName);
    LOGI("vtable %s: update(slot %zu)=%p getLocalPlayer(slot %zu)=%p", kClientInstance,
        kSlotUpdate, reinterpret_cast<void*>(gAddresses[0]),
        kSlotGetLocalPlayer, reinterpret_cast<void*>(gAddresses[1]));
    return gAddresses[0] != 0 && gAddresses[1] != 0;
}

std::uintptr_t resolve(Id id) {
    return gAddresses[static_cast<int>(id)];
}

}