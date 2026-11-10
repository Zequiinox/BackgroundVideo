#pragma once

#include <filesystem>
#include <jni.h>

namespace bgv::video {

// Called from load(): remembers where the video and config live. No game access yet.
void setup(JavaVM* vm, const std::filesystem::path& resourceDir, const std::filesystem::path& configDir);

// Hooks eglSwapBuffers and the GL draw calls used to replace the panorama.
bool installHooks();
void removeHooks();

// Opens the video on a background thread so it is ready by the time the menu shows.
void preload();

void setEnabled(bool enabled);

}
