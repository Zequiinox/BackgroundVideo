#pragma once

namespace bgv::game {

// Hooks ClientInstance::update to learn whether a world is loaded (needs libminecraftpe.so).
bool install();
void uninstall();
bool installed();

// True once the game thread has reported at least once.
bool known();
// True while no local player exists (main menu and menu-like screens).
bool inMenu();

}
