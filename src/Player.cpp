#include "Player.hpp"
#include "Log.hpp"

namespace bgv {
namespace {

JavaVM* gVm = nullptr;

JNIEnv* currentEnv() {
    if (!gVm) return nullptr;
    JNIEnv* env = nullptr;
    const jint status = gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
#ifdef __ANDROID__
        if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
#else
        if (gVm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) return nullptr;
#endif
    } else if (status != JNI_OK) {
        return nullptr;
    }
    return env;
}

bool javaOk(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) return true;
    LOGE("Java exception in %s", what);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return false;
}

}

void Player::setJavaVm(JavaVM* vm) {
    gVm = vm;
}

void Player::detachThread() {
    if (gVm) gVm->DetachCurrentThread();
}

bool Player::prepare(const std::string& path, float volume, bool loop) {
    JNIEnv* env = currentEnv();
    if (!env) return false;

    auto cls = [&](const char* name) -> jclass {
        jclass local = env->FindClass(name);
        if (!local || !javaOk(env, name)) return nullptr;
        auto global = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        return global;
    };
    mSurfaceTextureClass = cls("android/graphics/SurfaceTexture");
    mSurfaceClass = cls("android/view/Surface");
    mPlayerClass = cls("android/media/MediaPlayer");
    if (!mSurfaceTextureClass || !mSurfaceClass || !mPlayerClass) return fail(env, "class lookup");

    auto method = [&](jclass c, const char* name, const char* sig) {
        jmethodID id = env->GetMethodID(c, name, sig);
        javaOk(env, name);
        return id;
    };
    // SurfaceTexture(boolean) creates the texture detached from any GL context (Android 8+),
    // so the video can be opened long before the game has a GL context.
    const jmethodID stCtor = method(mSurfaceTextureClass, "<init>", "(Z)V");
    const jmethodID surfaceCtor = method(mSurfaceClass, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
    const jmethodID playerCtor = method(mPlayerClass, "<init>", "()V");
    mAttachToGlContext = method(mSurfaceTextureClass, "attachToGLContext", "(I)V");
    mUpdateTexImage = method(mSurfaceTextureClass, "updateTexImage", "()V");
    mGetTimestamp = method(mSurfaceTextureClass, "getTimestamp", "()J");
    mGetTransformMatrix = method(mSurfaceTextureClass, "getTransformMatrix", "([F)V");
    mSurfaceTextureRelease = method(mSurfaceTextureClass, "release", "()V");
    mSurfaceRelease = method(mSurfaceClass, "release", "()V");
    const jmethodID setDataSource = method(mPlayerClass, "setDataSource", "(Ljava/lang/String;)V");
    const jmethodID setSurface = method(mPlayerClass, "setSurface", "(Landroid/view/Surface;)V");
    const jmethodID setVolume = method(mPlayerClass, "setVolume", "(FF)V");
    const jmethodID setLooping = method(mPlayerClass, "setLooping", "(Z)V");
    const jmethodID prepareMethod = method(mPlayerClass, "prepare", "()V");
    mPlayerStart = method(mPlayerClass, "start", "()V");
    mPlayerPause = method(mPlayerClass, "pause", "()V");
    mPlayerStop = method(mPlayerClass, "stop", "()V");
    mPlayerRelease = method(mPlayerClass, "release", "()V");
    const jmethodID getDuration = method(mPlayerClass, "getDuration", "()I");
    const jmethodID getVideoWidth = method(mPlayerClass, "getVideoWidth", "()I");
    const jmethodID getVideoHeight = method(mPlayerClass, "getVideoHeight", "()I");
    if (!stCtor || !surfaceCtor || !playerCtor || !mAttachToGlContext || !mUpdateTexImage || !mGetTimestamp
        || !mGetTransformMatrix || !setDataSource || !setSurface || !prepareMethod || !mPlayerStart || !mPlayerPause) {
        return fail(env, "method lookup (needs Android 8+)");
    }

    jobject st = env->NewObject(mSurfaceTextureClass, stCtor, JNI_FALSE);
    if (!st || !javaOk(env, "SurfaceTexture")) return fail(env, "SurfaceTexture");
    mSurfaceTexture = env->NewGlobalRef(st);
    env->DeleteLocalRef(st);

    jobject surface = env->NewObject(mSurfaceClass, surfaceCtor, mSurfaceTexture);
    if (!surface || !javaOk(env, "Surface")) return fail(env, "Surface");
    mSurface = env->NewGlobalRef(surface);
    env->DeleteLocalRef(surface);

    jobject player = env->NewObject(mPlayerClass, playerCtor);
    if (!player || !javaOk(env, "MediaPlayer")) return fail(env, "MediaPlayer");
    mPlayer = env->NewGlobalRef(player);
    env->DeleteLocalRef(player);

    jstring jpath = env->NewStringUTF(path.c_str());
    env->CallVoidMethod(mPlayer, setDataSource, jpath);
    env->DeleteLocalRef(jpath);
    if (!javaOk(env, "setDataSource")) return fail(env, "setDataSource");
    env->CallVoidMethod(mPlayer, setSurface, mSurface);
    if (!javaOk(env, "setSurface")) return fail(env, "setSurface");
    if (setLooping) env->CallVoidMethod(mPlayer, setLooping, loop ? JNI_TRUE : JNI_FALSE);
    if (setVolume) env->CallVoidMethod(mPlayer, setVolume, volume, volume);
    javaOk(env, "volume/looping");
    env->CallVoidMethod(mPlayer, prepareMethod); // blocks; this is why it runs on a helper thread
    if (!javaOk(env, "prepare")) return fail(env, "prepare");

    mDuration = getDuration ? env->CallIntMethod(mPlayer, getDuration) : 0;
    mWidth = getVideoWidth ? env->CallIntMethod(mPlayer, getVideoWidth) : 0;
    mHeight = getVideoHeight ? env->CallIntMethod(mPlayer, getVideoHeight) : 0;
    javaOk(env, "video info");

    jfloatArray matrix = env->NewFloatArray(16);
    mMatrix = static_cast<jfloatArray>(env->NewGlobalRef(matrix));
    env->DeleteLocalRef(matrix);
    mPaused = false;
    LOGI("video prepared (%dx%d, %d ms)", mWidth, mHeight, mDuration);
    return true;
}

bool Player::attachAndStart(unsigned int texture) {
    JNIEnv* env = currentEnv();
    if (!env || !mPlayer || !mSurfaceTexture) return false;
    env->CallVoidMethod(mSurfaceTexture, mAttachToGlContext, static_cast<jint>(texture));
    if (!javaOk(env, "attachToGLContext")) return false;
    env->CallVoidMethod(mPlayer, mPlayerStart);
    if (!javaOk(env, "start")) return false;
    mPaused = false;
    return true;
}

bool Player::update(float matrix[16]) {
    JNIEnv* env = currentEnv();
    if (!env || !mSurfaceTexture) return false;
    env->CallVoidMethod(mSurfaceTexture, mUpdateTexImage);
    if (!javaOk(env, "updateTexImage")) return false;
    env->CallVoidMethod(mSurfaceTexture, mGetTransformMatrix, mMatrix);
    if (!javaOk(env, "getTransformMatrix")) return false;
    env->GetFloatArrayRegion(mMatrix, 0, 16, matrix);
    return true;
}

bool Player::frameReady() {
    JNIEnv* env = currentEnv();
    if (!env || !mSurfaceTexture) return false;
    const jlong timestamp = env->CallLongMethod(mSurfaceTexture, mGetTimestamp);
    javaOk(env, "getTimestamp");
    return timestamp > 0;
}

void Player::pause() {
    JNIEnv* env = currentEnv();
    if (!env || !mPlayer || mPaused) return;
    env->CallVoidMethod(mPlayer, mPlayerPause);
    javaOk(env, "pause");
    mPaused = true;
}

void Player::resume() {
    JNIEnv* env = currentEnv();
    if (!env || !mPlayer || !mPaused) return;
    env->CallVoidMethod(mPlayer, mPlayerStart);
    javaOk(env, "resume");
    mPaused = false;
}

void Player::release() {
    JNIEnv* env = currentEnv();
    if (!env) return;
    releaseAll(env);
}

bool Player::fail(JNIEnv* env, const char* step) {
    LOGE("player setup failed at: %s", step);
    releaseAll(env);
    return false;
}

void Player::releaseAll(JNIEnv* env) {
    if (mPlayer) {
        if (mPlayerStop) env->CallVoidMethod(mPlayer, mPlayerStop);
        javaOk(env, "stop");
        if (mPlayerRelease) env->CallVoidMethod(mPlayer, mPlayerRelease);
        javaOk(env, "release");
    }
    if (mSurface && mSurfaceRelease) {
        env->CallVoidMethod(mSurface, mSurfaceRelease);
        javaOk(env, "Surface.release");
    }
    if (mSurfaceTexture && mSurfaceTextureRelease) {
        env->CallVoidMethod(mSurfaceTexture, mSurfaceTextureRelease);
        javaOk(env, "SurfaceTexture.release");
    }
    auto drop = [&](jobject& ref) {
        if (ref) env->DeleteGlobalRef(ref);
        ref = nullptr;
    };
    drop(mPlayer);
    drop(mSurface);
    drop(mSurfaceTexture);
    if (mMatrix) env->DeleteGlobalRef(mMatrix);
    mMatrix = nullptr;
    if (mPlayerClass) env->DeleteGlobalRef(mPlayerClass);
    if (mSurfaceClass) env->DeleteGlobalRef(mSurfaceClass);
    if (mSurfaceTextureClass) env->DeleteGlobalRef(mSurfaceTextureClass);
    mPlayerClass = mSurfaceClass = mSurfaceTextureClass = nullptr;
    mPaused = false;
    mDuration = mWidth = mHeight = 0;
}

}
