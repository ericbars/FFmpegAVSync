#include "com_ffmpeg_avsync_VideoSurface.h"
#include <jni.h>

#include "player.h"

/*
 * Class:     com_ffmpeg_avsync_VideoSurface
 * Method:    setSurface
 * Signature: (Landroid/view/Surface;)I
 */JNIEXPORT jint JNICALL Java_com_ffmpeg_avsync_VideoSurface_setSurface(
		JNIEnv *env, jobject obj, jobject surface) {

	return setNativeSurface(env, obj, surface);
}

/*
 * Class:     com_ffmpeg_avsync_VideoSurface
 * Method:    nativePausePlayer
 * Signature: ()I
 */JNIEXPORT jint JNICALL Java_com_ffmpeg_avsync_VideoSurface_nativePausePlayer(
		JNIEnv *, jobject) {
	global_context.pause = 1;
	return 0;
}

/*
 * Class:     com_ffmpeg_avsync_VideoSurface
 * Method:    nativeResumePlayer
 * Signature: ()I
 */JNIEXPORT jint JNICALL Java_com_ffmpeg_avsync_VideoSurface_nativeResumePlayer(
		JNIEnv *, jobject) {
	global_context.pause = 0;
	return 0;
}

/*
 * Class:     com_ffmpeg_avsync_VideoSurface
 * Method:    nativeStopPlayer
 * Signature: ()I
 */JNIEXPORT jint JNICALL Java_com_ffmpeg_avsync_VideoSurface_nativeStopPlayer(
		JNIEnv *, jobject) {
	global_context.pause = 1;
	global_context.quit = 1;
	eglClose();
	destroyPlayerAndEngine();
	usleep(50000);
	return 0;
}
