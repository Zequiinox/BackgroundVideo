#include "Signatures.hpp"

#include <pl/memory/Signature.hpp>
#include <string>
#include <vector>

namespace bgv::signatures {
namespace {

// Byte patterns copied from BedrockTools (verified to match exactly once in the 26.62 libminecraftpe.so).
constexpr const char* kClientInstanceUpdate =
    "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 59 D0 3B D5 ? ? ? 52 F5 03 02 2A";
constexpr const char* kClientInstanceGetLocalPlayer =
    "? ? ? D1 ? ? ? A9 ? ? ? F9 ? ? ? 91 53 D0 3B D5 E8 03 00 AA ? ? ? 91 ? ? ? F9 ? ? ? 91 ? ? ? F8 ? ? ? ? ? ? ? 91 ? ? ? ? ? ? ? 36 ? ? ? 91 ? ? ? 52 ? ? ? ? ? ? ? ? E0 03 1F AA ? ? ? F9 ? ? ? F8 1F 01 09 EB ? ? ? 54 ? ? ? A9 ? ? ? F9 ? ? ? 91 C0 03 5F D6 ? ? ? ? C0 03 5F D6";

std::uintptr_t gAddresses[2]{};

}

bool resolveAll(const char* libraryName) {
    const std::vector<std::string> patterns{kClientInstanceUpdate, kClientInstanceGetLocalPlayer};
    const auto resolved = pl::memory::resolveSignatures(patterns, libraryName);
    gAddresses[0] = gAddresses[1] = 0;
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        const auto it = resolved.find(patterns[i]);
        if (it != resolved.end()) gAddresses[i] = it->second;
    }
    return gAddresses[0] != 0 && gAddresses[1] != 0;
}

std::uintptr_t resolve(Id id) {
    return gAddresses[static_cast<int>(id)];
}

}
