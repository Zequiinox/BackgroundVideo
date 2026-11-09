#pragma once

#include <jni.h>
#include <string>

namespace bgv {

// Android MediaPlayer + SurfaceTexture driven over JNI.
// Every call must happen on the thread that owns the GL context (the game's render thread).
class Player {
public:
    static void setJavaVm(JavaVM* vm);

    bool start(const std::string& path, unsigned int texture, float volume, bool loop);
    // Latches the newest decoded frame into the GL texture and returns its UV transform.
    bool update(float matrix[16]);
    void pause();
    void resume();
    void release();

    bool active() const { return mPlayer != nullptr; }
    bool paused() const { return mPaused; }
    int position();
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
    jmethodID mGetTransformMatrix = nullptr;
    jmethodID mSurfaceTextureRelease = nullptr;
    jmethodID mSurfaceRelease = nullptr;
    jmethodID mPlayerStop = nullptr;
    jmethodID mPlayerRelease = nullptr;
    jmethodID mPlayerPause = nullptr;
    jmethodID mPlayerStart = nullptr;
    jmethodID mGetPosition = nullptr;
    int mDuration = 0;
    int mWidth = 0;
    int mHeight = 0;
    bool mPaused = false;
};

}
