#include "BackgroundVideo.hpp"
#include "Game.hpp"
#include "Gl.hpp"
#include "Hooks.hpp"
#include "Log.hpp"
#include "Player.hpp"

#include <EGL/egl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bgv::video {
namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Config (<configDir>/config.json, created with defaults on first run)
// ---------------------------------------------------------------------------
struct Config {
    std::string videoPath;                // full path to a video or an image (png/jpg/webp/bmp/gif); defaults to <config folder>/menu.mp4
    std::string video = "menu.mp4";       // used only when video_path is empty: looked up in the config/resources folder
    float volume = 0.0f;                  // 0.0 (muted) - 1.0
    std::string fit = "cover";            // "cover" fills the screen (crops), "contain" shows black bars
    float brightness = 1.0f;              // 0.0 - 1.0, lower it if the menu text is hard to read
    bool debugTrace = false;              // log every draw call of three menu frames (for bug reports)
};

std::filesystem::path gResourceDir;
std::filesystem::path gConfigDir;
Config gConfig;
std::filesystem::file_time_type gConfigTime{};

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
    read("debug_trace", gConfig.debugTrace);
    gConfig.volume = std::clamp(gConfig.volume, 0.0f, 1.0f);
    gConfig.brightness = std::clamp(gConfig.brightness, 0.0f, 1.0f);
    if (gConfig.fit != "cover" && gConfig.fit != "contain") gConfig.fit = "cover";
}

// Creates config.json with defaults only if it does not exist yet. A failure here is harmless.
void writeDefaultConfigIfMissing() {
    std::error_code ec;
    if (std::filesystem::exists(configFile(), ec)) return;
    nlohmann::json out = {
        {"video_path", gConfig.videoPath},
        {"video", gConfig.video},
        {"volume", gConfig.volume},
        {"fit", gConfig.fit},
        {"brightness", gConfig.brightness},
        {"debug_trace", gConfig.debugTrace},
    };
    std::filesystem::create_directories(gConfigDir, ec);
    std::ofstream file(configFile());
    if (file) file << out.dump(2) << "\n";
    readConfig();
}

// ---------------------------------------------------------------------------
// State. Only the render thread touches the GL objects; the player is prepared on a helper thread.
// ---------------------------------------------------------------------------
std::atomic_bool gEnabled{false};
std::atomic_bool gReleaseRequested{false};

// Video preparation (opening the file, setting up the decoder) can take a second: never on the render thread.
enum Prep : int { PrepNone = 0, PrepBusy = 1, PrepReady = 2, PrepFailed = 3 };
std::atomic<int> gPrep{PrepNone};
std::atomic_bool gDiscard{false}; // release the player as soon as the helper thread is done
bool gPrepImage = false;          // the file being prepared is an image (written before the helper thread starts)
std::atomic_bool gIsImage{false}; // the current media is a still image (read by the watchdog thread)
Image gImage;

Player gPlayer;
gl::Renderer gRenderer;
EGLContext gContext = EGL_NO_CONTEXT;
std::atomic_bool gStarted{false}; // attached to the GL context and playing (read by the watchdog thread)
bool gFirstFrame = false;    // a decoded frame has been latched at least once
bool gFailed = false;        // unrecoverable (shader problem)
bool gFrameHandled = false;  // we already decided what to do with the panorama this frame
bool gFrameShown = false;    // ...and we drew the video this frame
int gUpdateFailures = 0;
int gForeignSwaps = 0;       // consecutive frames swapped by a context that is not ours
int gRetries = 0;
long gFrameCount = 0;
long gLastPanoFrame = -1000;  // frame in which a panorama face was last drawn
bool gPanoThisFrame = false;
bool gLastFramePano = false;
int gTransitionLogs = 0;
int gTraceFramesLeft = 0;
long gMenuFrames = 0;
long gPanoramaDraws = 0;
long gDrawsInMenu[5]{};      // elements, arrays, elementsInstanced, arraysInstanced, rangeElements
bool gLoggedReplace = false;
bool gLoggedNoPanorama = false;
bool gLoggedGl = false;
bool gTraceFrame = false;
int gProgramsLogged = 0;
std::atomic<long long> gLastSwapMs{0}; // when the render thread last finished a frame
std::atomic_bool gWatchdogRun{false};
Clock::time_point gNextRetry{};
Clock::time_point gLastSeen{};
std::filesystem::path gPrepPath;       // the file being (or last) prepared
std::filesystem::file_time_type gPrepTime{};
std::filesystem::path gLoadedPath;     // the file that is playing
std::filesystem::file_time_type gLoadedTime{};
std::unordered_map<GLuint, bool> gProgramCache;

// On this build the panorama is six separate quads (index count 6), each with its own 2D texture, and they are
// the first draws of every menu frame. The mod learns those six texture ids from the start of each menu frame.
constexpr int kLeadingDraws = 16; // the panorama is always among the first draws of a menu frame
std::vector<GLint> gPanoTextures;
std::vector<GLint> gLearning;
GLint gLearnProgram = 0;
int gFrameDraws = 0;
int gNoPanoFrames = 0;
bool gLearnOpen = true;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// When the app goes to the background the render thread stops swapping buffers. This thread notices and pauses
// the video, so it is exactly where you left it when you come back.
void startWatchdog() {
    if (gWatchdogRun.exchange(true)) return;
    std::thread([] {
        while (gWatchdogRun.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const long long last = gLastSwapMs.load();
            if (last > 0 && gStarted.load() && !gIsImage.load() && !gPlayer.paused() && nowMs() - last > 1000) {
                LOGI("no frames for 1s (app in the background?) - pausing the video");
                gPlayer.pause();
            }
        }
        Player::detachThread();
    }).detach();
}

bool isImagePath(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp" || ext == ".gif";
}

void releaseMedia() {
    gPlayer.release();
    gImage.release();
}

// ---------------------------------------------------------------------------
// Background preparation
// ---------------------------------------------------------------------------
void startPrepare(const std::filesystem::path& path) {
    int expected = PrepNone;
    if (!gPrep.compare_exchange_strong(expected, PrepBusy)) return; // already preparing / ready
    std::error_code ec;
    gPrepPath = path;
    gPrepTime = std::filesystem::last_write_time(path, ec);
    gDiscard = false;
    gPrepImage = isImagePath(path);
    const bool image = gPrepImage;
    const std::string file = path.string();
    const float volume = gConfig.volume;
    LOGI("preparing %s %s in the background", image ? "image" : "video", file.c_str());
    std::thread([file, volume, image] {
        const bool ok = image ? gImage.load(file, 2048) : gPlayer.prepare(file, volume, true);
        const bool discard = gDiscard.exchange(false);
        if (discard && ok) releaseMedia();
        Player::detachThread();
        gPrep = discard ? PrepNone : (ok ? PrepReady : PrepFailed);
    }).detach();
}

// Drops the player and GL objects. The GL context is still ours, so GL objects are deleted properly.
void releaseResources() {
    if (gPrep.load() == PrepBusy) {
        gDiscard = true;
    } else {
        releaseMedia();
        gPrep = PrepNone;
    }
    if (gRenderer.ready()) gRenderer.destroy();
    gIsImage = false;
    gStarted = gFirstFrame = false;
    gFrameHandled = gFrameShown = false;
    gContext = EGL_NO_CONTEXT;
}

// The GL context is gone (the app went to the background): only the Java side can be released.
void abandonResources() {
    if (gPrep.load() == PrepBusy) {
        gDiscard = true;
    } else {
        releaseMedia();
        gPrep = PrepNone;
    }
    gRenderer.forget();
    gIsImage = false;
    gStarted = gFirstFrame = false;
    gFrameHandled = gFrameShown = false;
    gContext = EGL_NO_CONTEXT;
    gNextRetry = Clock::now();
}

// ---------------------------------------------------------------------------
// Panorama detection
// ---------------------------------------------------------------------------
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

// Six quads in a row with the same program and six different textures are the panorama faces. The run may start
// anywhere in the first draws of a menu frame (the game can draw something before it, e.g. a fade quad), but it
// must be exactly six long, so a row of UI icons is not mistaken for it.
bool commitRun() {
    if (gLearning.size() != 6) return false;
    std::vector<GLint> unique = gLearning;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    if (unique.size() != 6 || unique.front() <= 0) return false;
    if (unique != gPanoTextures) {
        gPanoTextures = unique;
        LOGI("learned panorama faces: program %d, textures %d %d %d %d %d %d", gLearnProgram,
            unique[0], unique[1], unique[2], unique[3], unique[4], unique[5]);
    }
    return true;
}

void learnPanorama(GLint program, GLsizei count, int index) {
    if (!gLearnOpen) return;
    const bool quad = count == 6;
    if (quad && (gLearning.empty() || program == gLearnProgram)) {
        if (gLearning.empty()) gLearnProgram = program;
        gLearning.push_back(texture0());
    } else {
        if (commitRun()) gLearnOpen = false; // a finished run of exactly six different textures
        gLearning.clear();
        if (quad && gLearnOpen) { // this quad starts a new run
            gLearnProgram = program;
            gLearning.push_back(texture0());
        }
    }
    if (gLearnOpen && index + 1 >= kLeadingDraws) { // end of the window
        commitRun();
        gLearnOpen = false;
        gLearning.clear();
    }
}

// Fallback for builds that draw the panorama with a real cube map sampler.
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
        if (type >= 0x8B5E && type <= 0x8B62 && used < static_cast<int>(sizeof(samplers)) - 24)
            used += std::snprintf(samplers + used, sizeof(samplers) - used, "%s=0x%X ", name, type);
    }
    if (gProgramsLogged < 40) {
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

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
// Non-GL part of starting: makes sure the player is being prepared. True once it is ready to attach.
bool startupStep() {
    switch (gPrep.load()) {
        case PrepReady:
            return true;
        case PrepBusy:
            return false;
        case PrepFailed:
            gPrep = PrepNone;
            gNextRetry = Clock::now() + std::chrono::seconds(2);
            if (gRetries++ % 15 == 0) LOGW("could not play the video (is it a valid mp4?), will retry");
            return false;
        default: {
            const auto path = findVideo();
            if (path.empty()) {
                gNextRetry = Clock::now() + std::chrono::seconds(2);
                if (gRetries++ % 15 == 0) {
                    LOGW("cannot read a video or image at %s - set \"video_path\" in %s (the normal panorama stays)",
                        candidates().front().c_str(), configFile().c_str());
                }
                return false;
            }
            startPrepare(path);
            return false;
        }
    }
}

// Uploads the decoded image into the renderer's 2D texture (a GL context must be current, texture bound).
bool uploadImage() {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gRenderer.texture);
    return gImage.upload();
}

// GL part of starting, on the render thread: needs the media to be ready.
bool attachPlayback() {
    if (!gRenderer.create(gPrepImage)) {
        gRenderer.destroy();
        gFailed = true;
        return false;
    }
    const bool ok = gPrepImage ? uploadImage() : gPlayer.attachAndStart(gRenderer.texture);
    if (!ok) {
        gRenderer.destroy();
        releaseMedia();
        gPrep = PrepNone;
        gNextRetry = Clock::now() + std::chrono::seconds(2);
        return false;
    }
    gIsImage = gPrepImage;
    gLoadedPath = gPrepPath;
    gLoadedTime = gPrepTime;
    gContext = eglGetCurrentContext();
    gStarted = true;
    gFirstFrame = gPrepImage; // an image has its "frame" right away
    gRetries = 0;
    LOGI("showing %s %s", gPrepImage ? "image" : "video", gLoadedPath.c_str());
    return true;
}

// Draws the video into the current render target, exactly where the panorama would have gone.
bool renderVideo() {
    if (!gStarted && !startupStep()) return false; // still preparing: no GL work, the panorama stays

    gl::SavedState saved;
    gl::save(saved);
    bool shown = false;
    do {
        if (!gStarted) {
            if (!attachPlayback()) break;
        } else if (!gIsImage && gPlayer.paused()) {
            gPlayer.resume();
        }

        float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (gIsImage) {
            // bitmaps are stored top row first, so flip v
            const float flip[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1};
            std::copy(flip, flip + 16, matrix);
        } else if (!gPlayer.update(matrix)) {
            if (++gUpdateFailures > 60) {
                LOGW("video frames cannot be read, restarting");
                gUpdateFailures = 0;
                gReleaseRequested = true; // handled at the end of the frame
            }
            break;
        }
        gUpdateFailures = 0;
        if (!gFirstFrame) {
            if (!gPlayer.frameReady()) break; // no decoded frame yet: the panorama shows for a moment
            gFirstFrame = true;
        }

        const int width = saved.viewport[2], height = saved.viewport[3];
        if (width <= 0 || height <= 0) break;
        const float screenAspect = static_cast<float>(width) / static_cast<float>(height);
        const int vw = gIsImage ? gImage.width() : gPlayer.width();
        const int vh = gIsImage ? gImage.height() : gPlayer.height();
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
        glBindTexture(gRenderer.target, gRenderer.texture);
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

// Returns true when the game's own draw call must be skipped because it was part of the panorama.
bool replacePanorama(int fn, GLsizei count) {
    if (!gEnabled.load(std::memory_order_relaxed) || gFailed) return false;
    // The panorama is on screen in the main menu, but also behind other menu-like screens such as Profile, where the
    // game can have a local player (so "no local player" is not a reliable signal). Once the panorama has been seen,
    // keep looking for it for a second after the last sighting, wherever it shows up in the frame.
    const bool menu = game::known() && game::inMenu();
    const bool recentlySeen = gFrameCount - gLastPanoFrame < 60;
    if (!menu && !recentlySeen) return false;
    ++gDrawsInMenu[fn];
    if (gTraceFrame) {
        static const char* const names[] = {"glDrawElements", "glDrawArrays", "glDrawElementsInstanced", "glDrawArraysInstanced", "glDrawRangeElements"};
        GLint traceProgram = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &traceProgram);
        traceDraw(names[fn], count, traceProgram);
    }
    const int index = gFrameDraws++;
    if (gContext != EGL_NO_CONTEXT && eglGetCurrentContext() != gContext) return false;

    // Learning only happens from the leading draws of a real menu frame.
    const bool leading = menu && index < kLeadingDraws;
    GLint program = 0;
    if (leading) {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        if (!gLoggedGl) {
            gLoggedGl = true;
            LOGI("GL: %s | %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        }
        learnPanorama(program, count, index);
    }
    if (!gStarted && Clock::now() < gNextRetry) return false; // waiting before retrying a failed start

    // Detection by the learned face textures works at any position in the frame; only quads (count 6) are checked.
    bool panorama = count == 6 && !gPanoTextures.empty() && isPanoramaTexture(texture0());
    if (!panorama && leading) panorama = isCubemapProgram(static_cast<GLuint>(program));
    if (!panorama) return false;

    ++gPanoramaDraws;
    gPanoThisFrame = true;
    gLastPanoFrame = gFrameCount;
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

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    // End of frame: bookkeeping on the render thread (its GL context is current here).
    gLastSwapMs.store(nowMs());
    ++gFrameCount;
    gFrameHandled = false;
    gFrameShown = false;
    gFrameDraws = 0;
    gLearnOpen = true;
    gLearning.clear();

    const EGLContext context = eglGetCurrentContext();
    const bool ownContext = gContext == EGL_NO_CONTEXT || context == gContext;

    // The app was sent to the background and came back: Android destroyed our GL context and made a new one.
    // Keep the same MediaPlayer (so the video continues where it was) and just move it to the new context.
    if (gStarted && !ownContext && context != EGL_NO_CONTEXT) {
        if (++gForeignSwaps >= 5) {
            gForeignSwaps = 0;
            LOGI("GL context changed (app resumed) - moving the video to the new context");
            gl::SavedState saved;
            gl::save(saved);
            gRenderer.forget(); // the old objects died with the old context
            const bool ok = gIsImage ? (gRenderer.create(true) && uploadImage())
                                     : (gRenderer.create() && gPlayer.rebind(gRenderer.texture));
            gl::restore(saved);
            if (ok) {
                gContext = context;
                gFirstFrame = false;
                gUpdateFailures = 0; // playback resumes at the next panorama draw
            } else {
                LOGW("could not move the video, restarting it");
                gRenderer.forget();
                abandonResources();
                if (gEnabled.load()) {
                    const auto path = findVideo();
                    if (!path.empty()) startPrepare(path); // be ready again before the next panorama draw
                }
            }
        }
    } else {
        gForeignSwaps = 0;
    }

    if (gReleaseRequested.exchange(false, std::memory_order_acq_rel) && (gStarted || gPrep.load() != PrepNone)) {
        if (ownContext) releaseResources();
        else abandonResources();
        LOGI("video released");
    }

    const bool menu = game::known() && game::inMenu();
    // Log every time the panorama appears or disappears (e.g. when opening Profile) and, with debug_trace on,
    // trace the next two frames so a bug report shows what the screen is drawn like.
    if (gPanoThisFrame != gLastFramePano && gTransitionLogs < 40) {
        ++gTransitionLogs;
        LOGI("panorama %s (inMenu=%d, frame %ld)", gPanoThisFrame ? "visible" : "gone", menu ? 1 : 0, gFrameCount);
        if (gConfig.debugTrace && gTransitionLogs <= 8) gTraceFramesLeft = 2;
    }
    gLastFramePano = gPanoThisFrame;
    gPanoThisFrame = false;
    // The game can reload its textures (e.g. around loading a world), which gives the panorama new texture ids.
    // If menu frames keep passing without the known ids showing up, forget them so they are learned again.
    if (menu && !gPanoTextures.empty()) {
        if (gLastFramePano) gNoPanoFrames = 0;
        else if (++gNoPanoFrames == 120) {
            LOGI("panorama textures not seen for 120 menu frames - relearning them");
            gPanoTextures.clear();
        }
    } else {
        gNoPanoFrames = 0;
    }
    gTraceFrame = gConfig.debugTrace && ((menu && (gMenuFrames == 300 || gMenuFrames == 301 || gMenuFrames == 302)) || gTraceFramesLeft > 0);
    if (gTraceFramesLeft > 0) --gTraceFramesLeft;
    if (gTraceFrame && gMenuFrames == 300) LOGI("--- tracing 3 menu frames ---");
    if ((gFrameCount % 600) == 0 && gFrameCount <= 18000) {
        LOGI("stat frames=%ld known=%d inMenu=%d menuFrames=%ld draws[el=%ld ar=%ld elI=%ld arI=%ld rng=%ld] panoramaDraws=%ld faces=%zu started=%d prep=%d failed=%d",
            gFrameCount, game::known() ? 1 : 0, game::inMenu() ? 1 : 0, gMenuFrames, gDrawsInMenu[0], gDrawsInMenu[1],
            gDrawsInMenu[2], gDrawsInMenu[3], gDrawsInMenu[4], gPanoramaDraws, gPanoTextures.size(), gStarted ? 1 : 0, gPrep.load(), gFailed ? 1 : 0);
    }
    if (menu) {
        ++gMenuFrames;
        if (!gLoggedNoPanorama && gPanoramaDraws == 0 && gMenuFrames > 600) {
            gLoggedNoPanorama = true;
            LOGW("in menu for %ld frames but the panorama was never recognised (set debug_trace to true for a draw trace)", gMenuFrames);
        }
    }

    // Leaving the menu (or the panorama is gone): pause so it costs nothing in the world.
    if (gStarted && !gIsImage && !gPlayer.paused() && gLastSeen != Clock::time_point{}
        && Clock::now() - gLastSeen > std::chrono::milliseconds(600)) {
        gPlayer.pause();
    }

    // Hot reload: editing the config or replacing the video file restarts playback with the new one.
    if (menu && (gFrameCount % 90) == 0) {
        std::error_code configError;
        const auto configTime = std::filesystem::last_write_time(configFile(), configError);
        if (!configError && configTime != gConfigTime) {
            readConfig();
            LOGI("config reloaded");
        }
        if (gStarted && ownContext) {
            const auto current = findVideo();
            std::error_code ec;
            const auto time = current.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(current, ec);
            if (current != gLoadedPath || time != gLoadedTime) {
                LOGI("video changed, reloading");
                releaseResources();
                gNextRetry = Clock::now() + std::chrono::milliseconds(1500); // let a file copy finish
            }
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
    gConfig.videoPath = (configDir / "menu.mp4").string(); // default; overridden by config.json
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
    startWatchdog();
    LOGI("hooks installed (%zu)", gHandles.size());
    return true;
}

void removeHooks() {
    gWatchdogRun = false;
    for (auto handle : gHandles) hooks::remove(handle);
    gHandles.clear();
}

// Opens the video in the background while Minecraft is still starting, so it is ready when the menu appears.
void preload() {
    if (!gEnabled.load() || gPrep.load() != PrepNone) return;
    const auto path = findVideo();
    if (!path.empty()) startPrepare(path);
    else LOGI("preload: no video or image found yet, will look again when the menu shows");
}

void setEnabled(bool enabled) {
    gEnabled.store(enabled, std::memory_order_release);
    if (!enabled) gReleaseRequested.store(true, std::memory_order_release);
}

} // namespace bgv::video
