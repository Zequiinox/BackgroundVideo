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
    mStCtor = method(mSurfaceTextureClass, "<init>", "(Z)V");
    mSurfaceCtor = method(mSurfaceClass, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
    const jmethodID playerCtor = method(mPlayerClass, "<init>", "()V");
    mAttachToGlContext = method(mSurfaceTextureClass, "attachToGLContext", "(I)V");
    mUpdateTexImage = method(mSurfaceTextureClass, "updateTexImage", "()V");
    mGetTimestamp = method(mSurfaceTextureClass, "getTimestamp", "()J");
    mGetTransformMatrix = method(mSurfaceTextureClass, "getTransformMatrix", "([F)V");
    mSurfaceTextureRelease = method(mSurfaceTextureClass, "release", "()V");
    mSurfaceRelease = method(mSurfaceClass, "release", "()V");
    const jmethodID setDataSource = method(mPlayerClass, "setDataSource", "(Ljava/lang/String;)V");
    mSetSurface = method(mPlayerClass, "setSurface", "(Landroid/view/Surface;)V");
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
    if (!mStCtor || !mSurfaceCtor || !playerCtor || !mAttachToGlContext || !mUpdateTexImage || !mGetTimestamp
        || !mGetTransformMatrix || !setDataSource || !mSetSurface || !prepareMethod || !mPlayerStart || !mPlayerPause) {
        return fail(env, "method lookup (needs Android 8+)");
    }

    jobject st = env->NewObject(mSurfaceTextureClass, mStCtor, JNI_FALSE);
    if (!st || !javaOk(env, "SurfaceTexture")) return fail(env, "SurfaceTexture");
    mSurfaceTexture = env->NewGlobalRef(st);
    env->DeleteLocalRef(st);

    jobject surface = env->NewObject(mSurfaceClass, mSurfaceCtor, mSurfaceTexture);
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
    env->CallVoidMethod(mPlayer, mSetSurface, mSurface);
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
    std::lock_guard<std::mutex> lock(mMutex);
    JNIEnv* env = currentEnv();
    if (!env || !mPlayer || !mSurfaceTexture) return false;
    env->CallVoidMethod(mSurfaceTexture, mAttachToGlContext, static_cast<jint>(texture));
    if (!javaOk(env, "attachToGLContext")) return false;
    env->CallVoidMethod(mPlayer, mPlayerStart);
    if (!javaOk(env, "start")) return false;
    mPaused = false;
    return true;
}

bool Player::rebind(unsigned int texture) {
    std::lock_guard<std::mutex> lock(mMutex);
    JNIEnv* env = currentEnv();
    if (!env || !mPlayer || !mStCtor || !mSurfaceCtor || !mSetSurface) return false;

    jobject st = env->NewObject(mSurfaceTextureClass, mStCtor, JNI_FALSE);
    if (!st || !javaOk(env, "SurfaceTexture (rebind)")) return false;
    jobject newTexture = env->NewGlobalRef(st);
    env->DeleteLocalRef(st);
    jobject surface = env->NewObject(mSurfaceClass, mSurfaceCtor, newTexture);
    if (!surface || !javaOk(env, "Surface (rebind)")) {
        env->DeleteGlobalRef(newTexture);
        return false;
    }
    jobject newSurface = env->NewGlobalRef(surface);
    env->DeleteLocalRef(surface);

    env->CallVoidMethod(newTexture, mAttachToGlContext, static_cast<jint>(texture));
    bool ok = javaOk(env, "attachToGLContext (rebind)");
    if (ok) {
        env->CallVoidMethod(mPlayer, mSetSurface, newSurface);
        ok = javaOk(env, "setSurface (rebind)");
    }
    if (!ok) {
        env->DeleteGlobalRef(newSurface);
        env->DeleteGlobalRef(newTexture);
        return false;
    }

    // Drop the old surface; its GL context no longer exists, so errors here are expected and ignored.
    if (mSurface && mSurfaceRelease) {
        env->CallVoidMethod(mSurface, mSurfaceRelease);
        javaOk(env, "old Surface.release");
    }
    if (mSurfaceTexture && mSurfaceTextureRelease) {
        env->CallVoidMethod(mSurfaceTexture, mSurfaceTextureRelease);
        javaOk(env, "old SurfaceTexture.release");
    }
    if (mSurface) env->DeleteGlobalRef(mSurface);
    if (mSurfaceTexture) env->DeleteGlobalRef(mSurfaceTexture);
    mSurface = newSurface;
    mSurfaceTexture = newTexture;
    LOGI("video moved to the new GL context (still %s)", mPaused.load() ? "paused" : "playing");
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
    std::lock_guard<std::mutex> lock(mMutex);
    JNIEnv* env = currentEnv();
    if (!env || !mPlayer || mPaused) return;
    env->CallVoidMethod(mPlayer, mPlayerPause);
    javaOk(env, "pause");
    mPaused = true;
}

void Player::resume() {
    std::lock_guard<std::mutex> lock(mMutex);
    JNIEnv* env = currentEnv();
    if (!env || !mPlayer || !mPaused) return;
    env->CallVoidMethod(mPlayer, mPlayerStart);
    javaOk(env, "resume");
    mPaused = false;
}

void Player::release() {
    std::lock_guard<std::mutex> lock(mMutex);
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


bool Image::load(const std::string& path, int maxSize) {
    JNIEnv* env = currentEnv();
    if (!env) return false;
    release();

    jclass factory = env->FindClass("android/graphics/BitmapFactory");
    jclass bitmapClass = env->FindClass("android/graphics/Bitmap");
    if (!factory || !bitmapClass || !javaOk(env, "image classes")) return false;
    jmethodID decodeFile = env->GetStaticMethodID(factory, "decodeFile", "(Ljava/lang/String;)Landroid/graphics/Bitmap;");
    jmethodID getWidth = env->GetMethodID(bitmapClass, "getWidth", "()I");
    jmethodID getHeight = env->GetMethodID(bitmapClass, "getHeight", "()I");
    jmethodID recycle = env->GetMethodID(bitmapClass, "recycle", "()V");
    jmethodID scale = env->GetStaticMethodID(bitmapClass, "createScaledBitmap", "(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;");
    if (!decodeFile || !getWidth || !getHeight || !recycle || !scale || !javaOk(env, "image methods")) return false;

    jstring jpath = env->NewStringUTF(path.c_str());
    jobject bitmap = env->CallStaticObjectMethod(factory, decodeFile, jpath);
    env->DeleteLocalRef(jpath);
    if (!javaOk(env, "decodeFile") || !bitmap) {
        LOGE("cannot decode the image (unsupported or corrupt file?)");
        return false;
    }
    int width = env->CallIntMethod(bitmap, getWidth);
    int height = env->CallIntMethod(bitmap, getHeight);

    // Keep very large images within what the GPU handles comfortably.
    const int largest = width > height ? width : height;
    if (maxSize > 0 && largest > maxSize) {
        const float factor = static_cast<float>(maxSize) / static_cast<float>(largest);
        const int newWidth = static_cast<int>(static_cast<float>(width) * factor + 0.5f);
        const int newHeight = static_cast<int>(static_cast<float>(height) * factor + 0.5f);
        jobject scaled = env->CallStaticObjectMethod(bitmapClass, scale, bitmap, newWidth > 0 ? newWidth : 1, newHeight > 0 ? newHeight : 1, JNI_TRUE);
        if (javaOk(env, "createScaledBitmap") && scaled) {
            env->CallVoidMethod(bitmap, recycle);
            javaOk(env, "recycle");
            env->DeleteLocalRef(bitmap);
            bitmap = scaled;
            width = newWidth;
            height = newHeight;
        }
    }
    mBitmap = env->NewGlobalRef(bitmap);
    env->DeleteLocalRef(bitmap);
    mWidth = width;
    mHeight = height;
    LOGI("image decoded (%dx%d)", mWidth, mHeight);
    return mBitmap != nullptr;
}

bool Image::upload() {
    JNIEnv* env = currentEnv();
    if (!env || !mBitmap) return false;
    jclass glUtils = env->FindClass("android/opengl/GLUtils");
    if (!glUtils || !javaOk(env, "GLUtils")) return false;
    jmethodID texImage2D = env->GetStaticMethodID(glUtils, "texImage2D", "(IILandroid/graphics/Bitmap;I)V");
    if (!texImage2D || !javaOk(env, "texImage2D")) return false;
    env->CallStaticVoidMethod(glUtils, texImage2D, 0x0DE1 /* GL_TEXTURE_2D */, 0, mBitmap, 0);
    return javaOk(env, "texImage2D call");
}

void Image::release() {
    if (!mBitmap) return;
    JNIEnv* env = currentEnv();
    if (env) {
        jclass bitmapClass = env->FindClass("android/graphics/Bitmap");
        if (bitmapClass && javaOk(env, "Bitmap class")) {
            jmethodID recycle = env->GetMethodID(bitmapClass, "recycle", "()V");
            if (recycle) env->CallVoidMethod(mBitmap, recycle);
            javaOk(env, "recycle");
        }
        env->DeleteGlobalRef(mBitmap);
    }
    mBitmap = nullptr;
    mWidth = mHeight = 0;
}

}
