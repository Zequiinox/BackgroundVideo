#include "BackgroundVideo.hpp"
#include "Game.hpp"
#include "Gl.hpp"
#include "Hooks.hpp"
#include "Log.hpp"
#include "Player.hpp"

#include <EGL/egl.h>
#include "Log.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>


namespace bgv::video {
namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Config (<configDir>/config.json, created with defaults on first run)
// ---------------------------------------------------------------------------
struct Config {
    std::string videoPath;                // full path to your video, e.g. /storage/emulated/0/Movies/menu.mp4
    std::string video = "background.mp4"; // used only when video_path is empty: looked up in the config/resources folder
    float volume = 0.0f;                  // 0.0 (muted) - 1.0
    std::string fit = "cover";            // "cover" fills the screen (crops), "contain" shows black bars
    float brightness = 1.0f;              // 0.0 - 1.0, lower it if the menu text is hard to read
};

std::filesystem::path gResourceDir;
std::filesystem::path gConfigDir;
Config gConfig;

bool readable(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

// The video to play: the configured path, or (if none is set) background.mp4 in the config/resources folder.
// Files are only ever read, never written.
std::vector<std::filesystem::path> candidates() {
    std::vector<std::filesystem::path> list;
    if (!gConfig.videoPath.empty()) {
        list.emplace_back(gConfig.videoPath);
        return list;
    }
    const std::filesystem::path name(gConfig.video);
    if (name.is_absolute()) {
        list.push_back(name);
        return list;
    }
    list.push_back(gConfigDir / name);
    list.push_back(gResourceDir / name);
    return list;
}

std::filesystem::path findVideo() {
    for (const auto& path : candidates()) {
        if (readable(path)) return path;
    }
    return {};
}

std::filesystem::file_time_type gConfigTime{};

std::filesystem::path configFile() { return gConfigDir / "config.json"; }

void readConfig() {
    std::error_code ec;
    gConfigTime = std::filesystem::last_write_time(configFile(), ec);
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream in(configFile());
        if (in) {
            auto parsed = nlohmann::json::parse(in, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) j = std::move(parsed);
        }
    }
    auto read = [&](const char* key, auto& out) {
        try {
            if (j.contains(key)) out = j[key].get<std::remove_reference_t<decltype(out)>>();
        } catch (...) {}
    };
    read("video_path", gConfig.videoPath);
    read("video", gConfig.video);
    read("volume", gConfig.volume);
    read("fit", gConfig.fit);
    read("brightness", gConfig.brightness);
    gConfig.volume = std::clamp(gConfig.volume, 0.0f, 1.0f);
    gConfig.brightness = std::clamp(gConfig.brightness, 0.0f, 1.0f);
    if (gConfig.fit != "cover" && gConfig.fit != "contain") gConfig.fit = "cover";
}

// Creates config.json with defaults only if it does not exist yet. A failure here is harmless.
void writeDefaultConfigIfMissing() {
    std::error_code ec;
    if (std::filesystem::exists(configFile(), ec)) return;
    nlohmann::json out = {
        {"video_path", ""},
        {"video", gConfig.video},
        {"volume", gConfig.volume},
        {"fit", gConfig.fit},
        {"brightness", gConfig.brightness},
    };
    std::filesystem::create_directories(gConfigDir, ec);
    std::ofstream file(configFile());
    if (file) file << out.dump(2) << "\n";
    readConfig();
}

// ---------------------------------------------------------------------------
// Render-thread state
// ---------------------------------------------------------------------------
std::atomic_bool gEnabled{false};
std::atomic_bool gReleaseRequested{false};

Player gPlayer;
gl::Renderer gRenderer;
EGLContext gContext = EGL_NO_CONTEXT;
bool gStarted = false;
bool gFailed = false;
bool gFrameHandled = false; // we already decided what to do with the panorama this frame
bool gFrameShown = false;   // ...and we drew the video this frame
int gUpdateFailures = 0;
bool gLoggedReplace = false;
bool gLoggedNoCubemap = false;
Clock::time_point gNextRetry{};       // after a failed start, wait before trying again
int gRetries = 0;
std::filesystem::path gLoadedPath;    // the file that is currently playing
std::filesystem::file_time_type gLoadedTime{};
long gFrameCount = 0;
long gMenuFrames = 0;
long gCubemapDraws = 0;
long gDrawsInMenu[5]{};            // elements, arrays, elementsInstanced, arraysInstanced, rangeElements
bool gTraceFrame = false;          // log every draw call of a few menu frames
bool gLoggedGl = false;
int gProgramsLogged = 0;
// On this build the panorama is six separate quads (index count 6), each with its own 2D texture, drawn first in
// every menu frame. The mod learns those six texture ids from the start of each menu frame.
std::vector<GLint> gPanoTextures;   // texture ids of the six faces (last time they were seen)
std::vector<GLint> gLearning;       // textures of the leading quads of the current frame
GLint gLearnProgram = 0;
int gFrameDraws = 0;                // draws seen so far this frame (menu only)
bool gLearnOpen = true;             // still collecting this frame's leading run
Clock::time_point gLastSeen{};
std::unordered_map<GLuint, bool> gProgramCache;

// The main menu panorama is the only thing in the menu drawn with a cube map sampler.
bool isCubemapProgram(GLuint program) {
    if (!program) return false;
    const auto cached = gProgramCache.find(program);
    if (cached != gProgramCache.end()) return cached->second;
    bool cube = false;
    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    char samplers[160]{};
    int used = 0;
    for (GLint i = 0; i < count; ++i) {
        char name[96]{};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, i, sizeof(name) - 1, &length, &size, &type, name);
        if (type == GL_SAMPLER_CUBE || type == GL_SAMPLER_CUBE_SHADOW) cube = true;
        if (type >= 0x8B5E && type <= 0x8B62 && used < static_cast<int>(sizeof(samplers)) - 24) // sampler types
            used += std::snprintf(samplers + used, sizeof(samplers) - used, "%s=0x%X ", name, type);
    }
    if (gProgramsLogged < 60) {
        ++gProgramsLogged;
        LOGI("menu program %u: %d uniforms, cubemap=%d, samplers: %s", program, count, cube ? 1 : 0, samplers);
    }
    if (gProgramCache.size() > 1024) gProgramCache.clear();
    gProgramCache[program] = cube;
    return cube;
}

void traceDraw(const char* fn, GLsizei count, GLint program) {
    GLint fbo = 0, active = 0, viewport[4]{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    char units[160]{};
    int used = 0;
    for (int unit = 0; unit < 4; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        GLint tex2d = 0, cube = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &cube);
        used += std::snprintf(units + used, sizeof(units) - used, "u%d(2d=%d cube=%d) ", unit, tex2d, cube);
    }
    glActiveTexture(static_cast<GLenum>(active));
    LOGI("trace %s prog=%d fbo=%d vp=%dx%d count=%d %s", fn, program, fbo, viewport[2], viewport[3], count, units);
}

GLint texture0() {
    GLint active = 0, texture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glActiveTexture(static_cast<GLenum>(active));
    return texture;
}

bool isPanoramaTexture(GLint texture) {
    return std::find(gPanoTextures.begin(), gPanoTextures.end(), texture) != gPanoTextures.end();
}

// Looks at the leading draws of a menu frame: six quads with the same program and six different textures
// are the panorama faces. Called for every menu draw; only the first six per frame do any work.
void learnPanorama(GLint program, GLsizei count) {
    if (!gLearnOpen) return;
    const int index = gFrameDraws - 1;
    const bool conforming = count == 6 && index < 6 && (index == 0 || program == gLearnProgram);
    if (!conforming) {
        gLearnOpen = false;
        return;
    }
    if (index == 0) {
        gLearning.clear();
        gLearnProgram = program;
    }
    gLearning.push_back(texture0());
    if (index < 5) return;
    gLearnOpen = false;
    std::vector<GLint> unique = gLearning;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    if (unique.size() != 6 || unique.front() <= 0) return; // not six different textures: not the panorama
    if (unique != gPanoTextures) {
        gPanoTextures = unique;
        LOGI("learned panorama faces: program %d, textures %d %d %d %d %d %d", program,
            unique[0], unique[1], unique[2], unique[3], unique[4], unique[5]);
    }
}

enum class Begin { Ok, Retry, Fatal };

Begin beginPlayback() {
    const auto path = findVideo();
    if (path.empty()) {
        if (gRetries++ % 15 == 0) {
            const auto wanted = candidates().front();
            LOGW("cannot read a video at %s - set \"video_path\" in %s (the normal panorama stays)", wanted.c_str(), configFile().c_str());
        }
        return Begin::Retry;
    }
    if (!gRenderer.create()) {
        gRenderer.destroy();
        return Begin::Fatal;
    }
    if (!gPlayer.start(path.string(), gRenderer.texture, gConfig.volume, true)) {
        gRenderer.destroy();
        if (gRetries++ % 15 == 0) LOGW("could not play %s (is it a valid mp4?), will retry", path.c_str());
        return Begin::Retry;
    }
    std::error_code ec;
    gLoadedPath = path;
    gLoadedTime = std::filesystem::last_write_time(path, ec);
    gContext = eglGetCurrentContext();
    gStarted = true;
    gRetries = 0;
    LOGI("playing %s", path.c_str());
    return Begin::Ok;
}

// Draws the video into the current render target, exactly where the panorama would have gone.
bool renderVideo() {
    gl::SavedState saved;
    gl::save(saved);
    bool shown = false;
    do {
        if (!gStarted) {
            const auto result = beginPlayback();
            if (result == Begin::Fatal) gFailed = true;
            if (result != Begin::Ok) {
                gNextRetry = Clock::now() + std::chrono::seconds(2);
                break;
            }
        } else if (gPlayer.paused()) {
            gPlayer.resume();
        }

        float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (!gPlayer.update(matrix)) {
            if (++gUpdateFailures > 60) gFailed = true;
            break;
        }
        gUpdateFailures = 0;
        if (gPlayer.position() <= 0) break; // no decoded frame yet: let the panorama show for a moment

        const int width = saved.viewport[2], height = saved.viewport[3];
        if (width <= 0 || height <= 0) break;
        const float screenAspect = static_cast<float>(width) / static_cast<float>(height);
        const int vw = gPlayer.width(), vh = gPlayer.height();
        const float videoAspect = (vw > 0 && vh > 0) ? static_cast<float>(vw) / static_cast<float>(vh) : screenAspect;
        const bool cover = gConfig.fit == "cover";
        const bool wider = videoAspect > screenAspect;
        float sx = 1.0f, sy = 1.0f;
        if (cover) { // fill the screen, cropping the overflow
            if (wider) sx = videoAspect / screenAspect;
            else sy = screenAspect / videoAspect;
        } else { // fit inside the screen, black bars on the other axis
            if (wider) sy = screenAspect / videoAspect;
            else sx = videoAspect / screenAspect;
        }

        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, gRenderer.texture);
        glBindSampler(0, 0);
        glUseProgram(gRenderer.program);
        glUniform1i(gRenderer.uSampler, 0);
        if (!cover) gRenderer.draw(1.0f, 1.0f, 0.0f, identity); // black bars
        gRenderer.draw(sx, sy, gConfig.brightness, matrix);
        shown = true;
    } while (false);
    gl::restore(saved);
    return shown;
}

// Returns true when the game's own draw call must be skipped because it was the panorama.
bool replacePanorama(int fn, GLsizei count) {
    if (!gEnabled.load(std::memory_order_relaxed) || gFailed) return false;
    if (!game::known() || !game::inMenu()) return false;
    if (gContext != EGL_NO_CONTEXT && eglGetCurrentContext() != gContext) return false;

    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    ++gDrawsInMenu[fn];
    if (!gLoggedGl) {
        gLoggedGl = true;
        LOGI("GL: %s | %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    }
    if (gTraceFrame) {
        static const char* const names[] = {"glDrawElements", "glDrawArrays", "glDrawElementsInstanced", "glDrawArraysInstanced", "glDrawRangeElements"};
        traceDraw(names[fn], count, program);
    }
    ++gFrameDraws;
    learnPanorama(program, count);
    if (!gStarted && Clock::now() < gNextRetry) return false; // waiting before retrying a failed start
    bool panorama = count == 6 && !gPanoTextures.empty() && isPanoramaTexture(texture0());
    if (!panorama) panorama = isCubemapProgram(static_cast<GLuint>(program)); // other builds: real cube map
    if (!panorama) return false;

    ++gCubemapDraws;
    gLastSeen = Clock::now();
    if (!gFrameHandled) {
        gFrameHandled = true;
        gFrameShown = renderVideo();
        if (gFrameShown && !gLoggedReplace) {
            gLoggedReplace = true;
            LOGI("panorama replaced by video (program %d)", program);
        }
    }
    return gFrameShown;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
void (*gDrawElementsOriginal)(GLenum, GLsizei, GLenum, const void*) = nullptr;
void (*gDrawArraysOriginal)(GLenum, GLint, GLsizei) = nullptr;
void (*gDrawElementsInstancedOriginal)(GLenum, GLsizei, GLenum, const void*, GLsizei) = nullptr;
void (*gDrawArraysInstancedOriginal)(GLenum, GLint, GLsizei, GLsizei) = nullptr;
void (*gDrawRangeElementsOriginal)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void*) = nullptr;
EGLBoolean (*gSwapOriginal)(EGLDisplay, EGLSurface) = nullptr;
std::vector<hooks::Handle> gHandles;

void drawElementsHook(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (replacePanorama(0, count)) return;
    if (gDrawElementsOriginal) gDrawElementsOriginal(mode, count, type, indices);
}

void drawArraysHook(GLenum mode, GLint first, GLsizei count) {
    if (replacePanorama(1, count)) return;
    if (gDrawArraysOriginal) gDrawArraysOriginal(mode, first, count);
}

void drawElementsInstancedHook(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances) {
    if (replacePanorama(2, count)) return;
    if (gDrawElementsInstancedOriginal) gDrawElementsInstancedOriginal(mode, count, type, indices, instances);
}

void drawArraysInstancedHook(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
    if (replacePanorama(3, count)) return;
    if (gDrawArraysInstancedOriginal) gDrawArraysInstancedOriginal(mode, first, count, instances);
}

void drawRangeElementsHook(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
    if (replacePanorama(4, count)) return;
    if (gDrawRangeElementsOriginal) gDrawRangeElementsOriginal(mode, start, end, count, type, indices);
}

void releaseResources() {
    gPlayer.release();
    gRenderer.destroy();
    gStarted = false;
    gFrameHandled = gFrameShown = false;
    gContext = EGL_NO_CONTEXT;
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    // End of frame: run the bookkeeping on the render thread (its GL context is current here).
    ++gFrameCount;
    gFrameHandled = false;
    gFrameShown = false;
    gFrameDraws = 0;
    gLearnOpen = true;

    if (gReleaseRequested.exchange(false, std::memory_order_acq_rel) && gStarted
        && (gContext == EGL_NO_CONTEXT || eglGetCurrentContext() == gContext)) {
        releaseResources();
        LOGI("video released");
    }

    const bool menu = game::known() && game::inMenu();
    gTraceFrame = menu && (gMenuFrames == 300 || gMenuFrames == 301 || gMenuFrames == 302);
    if (gMenuFrames == 300 && menu) LOGI("--- tracing 3 menu frames ---");
    if ((gFrameCount % 600) == 0 && gFrameCount <= 18000) {
        LOGI("stat frames=%ld known=%d inMenu=%d menuFrames=%ld draws[el=%ld ar=%ld elI=%ld arI=%ld rng=%ld] panoramaDraws=%ld faces=%zu started=%d failed=%d",
            gFrameCount, game::known() ? 1 : 0, game::inMenu() ? 1 : 0, gMenuFrames, gDrawsInMenu[0], gDrawsInMenu[1],
            gDrawsInMenu[2], gDrawsInMenu[3], gDrawsInMenu[4], gCubemapDraws, gPanoTextures.size(), gStarted ? 1 : 0, gFailed ? 1 : 0);
    }
    if (menu) {
        ++gMenuFrames;
        if (!gLoggedNoCubemap && gCubemapDraws == 0 && gMenuFrames > 600) {
            gLoggedNoCubemap = true;
            LOGW("in menu for %ld frames but the panorama was never recognised - see the trace above", gMenuFrames);
        }
    }

    // Leaving the menu (or the panorama is gone): pause so it costs nothing in the world.
    if (gStarted && !gPlayer.paused() && gLastSeen != Clock::time_point{}
        && Clock::now() - gLastSeen > std::chrono::milliseconds(600)) {
        gPlayer.pause();
    }
    // Hot reload: replacing or deleting the video file restarts playback with the new one.
    if (menu && (gFrameCount % 90) == 0) {
        std::error_code configError;
        const auto configTime = std::filesystem::last_write_time(configFile(), configError);
        if (!configError && configTime != gConfigTime) {
            readConfig();
            LOGI("config reloaded");
        }
    }
    if (menu && gStarted && (gFrameCount % 90) == 0) {
        const auto current = findVideo();
        std::error_code ec;
        const auto time = current.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(current, ec);
        if (current != gLoadedPath || time != gLoadedTime) {
            LOGI("video changed, reloading");
            releaseResources();
            gNextRetry = Clock::now() + std::chrono::milliseconds(1500); // let a file copy finish
        }
    }
    if ((gFrameCount % 600) == 0) gProgramCache.clear(); // program ids can be reused by the game

    return gSwapOriginal ? gSwapOriginal(display, surface) : EGL_FALSE;
}

// Hooks an entry point in libGLESv2.so and, if it lives at a different address, in libGLESv3.so too.
void hookGl(const char* name, void* detour, void** original) {
    void* first = nullptr;
    for (const char* library : {"libGLESv2.so", "libGLESv3.so"}) {
        void* target = hooks::symbolFromLibrary(library, name);
        if (!target) {
            LOGW("%s: not exported by %s", name, library);
            continue;
        }
        if (target == first) continue; // same function, already hooked
        if (auto handle = hooks::install(target, detour, original)) {
            gHandles.push_back(handle);
            LOGI("hooked %s in %s", name, library);
        } else {
            LOGW("failed to hook %s in %s", name, library);
        }
        if (!first) first = target;
    }
}

} // namespace

void setup(JavaVM* vm, const std::filesystem::path& resourceDir, const std::filesystem::path& configDir) {
    Player::setJavaVm(vm);
    gResourceDir = resourceDir;
    gConfigDir = configDir;
    log::setFile(configDir / "log.txt");
    LOGI("BackgroundVideo 1.0.0 starting");
    readConfig();
    writeDefaultConfigIfMissing();
    LOGI("config file: %s", configFile().c_str());
}

bool installHooks() {
    if (!gHandles.empty()) return true;
    void* egl = hooks::symbolFromLibrary("libEGL.so", "eglSwapBuffers");
    if (egl) {
        if (auto handle = hooks::install(egl, reinterpret_cast<void*>(swapBuffersDetour), reinterpret_cast<void**>(&gSwapOriginal)))
            gHandles.push_back(handle);
    }
    if (gHandles.empty()) {
        LOGE("failed to hook eglSwapBuffers");
        return false;
    }
    hookGl("glDrawElements", reinterpret_cast<void*>(drawElementsHook), reinterpret_cast<void**>(&gDrawElementsOriginal));
    hookGl("glDrawArrays", reinterpret_cast<void*>(drawArraysHook), reinterpret_cast<void**>(&gDrawArraysOriginal));
    hookGl("glDrawElementsInstanced", reinterpret_cast<void*>(drawElementsInstancedHook), reinterpret_cast<void**>(&gDrawElementsInstancedOriginal));
    hookGl("glDrawArraysInstanced", reinterpret_cast<void*>(drawArraysInstancedHook), reinterpret_cast<void**>(&gDrawArraysInstancedOriginal));
    hookGl("glDrawRangeElements", reinterpret_cast<void*>(drawRangeElementsHook), reinterpret_cast<void**>(&gDrawRangeElementsOriginal));
    LOGI("hooks installed (%zu)", gHandles.size());
    return true;
}

void removeHooks() {
    for (auto handle : gHandles) hooks::remove(handle);
    gHandles.clear();
}

void setEnabled(bool enabled) {
    gEnabled.store(enabled, std::memory_order_release);
    if (!enabled) gReleaseRequested.store(true, std::memory_order_release);
}

} // namespace bgv::video
