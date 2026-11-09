#pragma once

#include <dlfcn.h>
#include <new>
#include <pl/memory/Hook.hpp>

namespace bgv::hooks {

struct State {
    void* target;
    void* detour;
};
using Handle = State*;

inline void* symbolFromLibrary(const char* libraryName, const char* name) {
    void* lib = dlopen(libraryName, RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen(libraryName, RTLD_NOW);
    if (!lib) return nullptr;
    void* sym = dlsym(lib, name);
    dlclose(lib);
    return sym;
}

inline Handle install(void* target, void* detour, void** original) {
    if (!target || !detour) return nullptr;
    if (pl::memory::hook(target, detour, original) != 0) return nullptr;
    auto* state = new (std::nothrow) State{target, detour};
    if (state) return state;
    pl::memory::unhook(target, detour);
    return nullptr;
}

inline void remove(Handle hook) {
    if (!hook) return;
    pl::memory::unhook(hook->target, hook->detour);
    delete hook;
}

}
