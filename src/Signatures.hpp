#pragma once

#include <cstdint>

namespace bgv::signatures {

enum class Id : int { ClientInstanceUpdate = 0, ClientInstanceGetLocalPlayer = 1 };

// Scans libminecraftpe.so; true only if both functions were found.
bool resolveAll(const char* libraryName);
std::uintptr_t resolve(Id id);

}
