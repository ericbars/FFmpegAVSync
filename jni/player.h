#ifndef __PLAYER_H__
#define __PLAYER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <android/log.h>
#include <time.h>
#include <utime.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sched.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "config.h"

#include "libavutil/log.h"
#include "libavutil/time.h"
#include "libavutil/samplefmt.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include <libavutil/imgutils.h>

#if CONFIG_AVDEVICE
#include "libavdevice/avdevice.h"
#endif
#if CONFIG_AVFILTER
#include "libavfilter/avfilter.h"
#endif

#ifndef UINT64_C
#define UINT64_C uint64_t
#endif

#include <android/log.h>

#define VIDEO_PICTURE_QUEUE_SIZE 30

enum {
	AV_SYNC_AUDIO_MASTER, AV_SYNC_VIDEO_MASTER, AV_SYNC_EXTERNAL_MASTER,
};

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	int abort_request;
	int serial;
	pthread_mutex_t mutex;
} PacketQueue;

typedef struct AudioParams {
	int freq;
	int channels;
	unsigned int channel_layout;
	enum AVSampleFormat fmt;
	int frame_size;
	int bytes_per_sec;
} AudioParams;

typedef struct VideoPicture {
	AVFrame *pFrame;
	int width, height;
	double pts;
} VideoPicture;

typedef struct GlobalContexts {
	// for egl
	EGLDisplay eglDisplay;
	EGLSurface eglSurface;
	EGLContext eglContext;
	EGLint eglFormat;

	// for opengl es
	GLuint mTextureID[3];
	GLuint glProgram;
	GLint positionLoc;

	// for av decode
	AVCodecContext *acodec_ctx;
	AVCodecContext *vcodec_ctx;
	AVStream *vstream;
	AVStream *astream;
	AVCodec *vcodec;
	AVCodec *acodec;

	// for av packet
	PacketQueue video_queue;
	PacketQueue audio_queue;

	// for av sync
	timer_t timer;

	pthread_mutex_t pictq_mutex;
	pthread_cond_t pictq_cond;
	int pictq_size;
	int pictq_windex;
	int pictq_rindex;
	VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
	int64_t video_current_pts_time;
	pthread_mutex_t timer_mutex;
	pthread_cond_t timer_cond;

	double frame_last_delay;
	double frame_last_pts;
	double frame_timer;

	int quit;
	int pause;
} GlobalContext;

double get_master_clock();
double get_audio_clock();
double get_video_clock();

void packet_queue_init(PacketQueue *q);
int packet_queue_get(PacketQueue *q, AVPacket *pkt);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);

int setNativeSurface(JNIEnv *env, jobject obj, jobject surface);
void* video_thread(void *argv);
void* picture_thread(void *argv);
void video_refresh_timer();
void schedule_refresh(int delay);
void* open_media(void *argv);
int32_t setBuffersGeometry(int32_t width, int32_t height);
void renderSurface(AVFrame *frame);
void Render(AVFrame *frame);
int CreateProgram();
int eglClose();
void destroyPlayerAndEngine();

int createEngine();
int createBufferQueueAudioPlayer();
void fireOnPlayer();


extern GlobalContext global_context;

#define TAG "FFmpeg"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)

#define TAG2 "OpenSLES"
#define LOGV2(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __PLAYER_H__ */

