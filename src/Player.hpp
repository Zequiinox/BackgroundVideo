#pragma once

#include <atomic>
#include <jni.h>
#include <mutex>
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
    // After the GL context was recreated: gives the same MediaPlayer a fresh SurfaceTexture on a new GL texture,
    // so playback continues from where it was instead of starting over.
    bool rebind(unsigned int texture);
    // Latches the newest decoded frame into the GL texture and returns its UV transform.
    bool update(float matrix[16]);
    // True once at least one decoded frame has been latched.
    bool frameReady();
    void pause();
    void resume();
    void release();

    bool active() const { return mPlayer != nullptr; }
    bool paused() const { return mPaused.load(); }
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
    std::atomic_bool mPaused{false};
    std::mutex mMutex; // pause()/resume() can be called from the watchdog thread
    jmethodID mStCtor = nullptr;
    jmethodID mSurfaceCtor = nullptr;
    jmethodID mSetSurface = nullptr;
};


// A still image decoded with Android's BitmapFactory and uploaded with GLUtils.
//  - load() may run on any thread.
//  - upload() must run on the GL thread, with the target GL_TEXTURE_2D texture bound on the active unit.
class Image {
public:
    bool load(const std::string& path, int maxSize);
    bool upload();
    void release();

    bool loaded() const { return mBitmap != nullptr; }
    int width() const { return mWidth; }
    int height() const { return mHeight; }

private:
    jobject mBitmap = nullptr;
    int mWidth = 0;
    int mHeight = 0;
};

}
