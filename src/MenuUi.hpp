#pragma once

#include <string>

namespace bgv::menu {
void registerMenu(const std::string& modId);
void unregisterMenu();
}