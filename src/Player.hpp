#pragma once

#include <jni.h>
#include <string>

namespace bgv {

// Android MediaPlayer + SurfaceTexture driven over JNI.
//  - prepare() may run on any thread (it blocks while the file is opened and the decoder is set up).
//  - everything else must run on the thread that owns the GL context (the game's render thread).
class Player {
public:
    static void setJavaVm(JavaVM* vm);
    static void detachThread(); // call before a helper thread that used prepare()/release() exits

    bool prepare(const std::string& path, float volume, bool loop);
    // Attaches the (so far detached) SurfaceTexture to a GL external texture and starts playback.
    bool attachAndStart(unsigned int texture);
    // Latches the newest decoded frame into the GL texture and returns its UV transform.
    bool update(float matrix[16]);
    // True once at least one decoded frame has been latched.
    bool frameReady();
    void pause();
    void resume();
    void release();

    bool active() const { return mPlayer != nullptr; }
    bool paused() const { return mPaused; }
    int width() const { return mWidth; }
    int height() const { return mHeight; }
    int durationMs() const { return mDuration; }

private:
    bool fail(JNIEnv* env, const char* step);
    void releaseAll(JNIEnv* env);

    jclass mSurfaceTextureClass = nullptr;
    jclass mSurfaceClass = nullptr;
    jclass mPlayerClass = nullptr;
    jobject mSurfaceTexture = nullptr;
    jobject mSurface = nullptr;
    jobject mPlayer = nullptr;
    jfloatArray mMatrix = nullptr;
    jmethodID mUpdateTexImage = nullptr;
    jmethodID mAttachToGlContext = nullptr;
    jmethodID mGetTimestamp = nullptr;
    jmethodID mGetTransformMatrix = nullptr;
    jmethodID mSurfaceTextureRelease = nullptr;
    jmethodID mSurfaceRelease = nullptr;
    jmethodID mPlayerStop = nullptr;
    jmethodID mPlayerRelease = nullptr;
    jmethodID mPlayerPause = nullptr;
    jmethodID mPlayerStart = nullptr;
    int mDuration = 0;
    int mWidth = 0;
    int mHeight = 0;
    bool mPaused = false;
};

}
