#include "player.h"

#define AV_SYNC_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0
/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20
/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

static double video_clock;
static double video_current_pts;

static int timer_delay_ms = 0;


double get_video_clock() {
	double delta = (av_gettime() - global_context.video_current_pts_time)
			/ 1000000.0;
	return video_current_pts + delta;
}

static double synchronize_video(AVFrame *pFrame, double pts) {
	double time_base;
	double frame_delay = 0;

	if (pts != 0) {
		video_clock = pts;
	} else {
		pts = video_clock;
	}

	time_base = av_q2d(global_context.vstream->time_base);
	frame_delay += (pFrame->repeat_pict * (time_base * 0.5));

	video_clock += frame_delay;

	return pts;
}

static int queue_picture(AVFrame *pFrame, double pts) {
	VideoPicture *vp;

	LOGV2("queue_picture : pFrame is %p", pFrame);
	pthread_mutex_lock(&global_context.pictq_mutex);
	while (global_context.pictq_size >= VIDEO_PICTURE_QUEUE_SIZE) {
		usleep(10000);
		LOGV2("global_context.pictq_size is %d", global_context.pictq_size);
		pthread_cond_wait(&global_context.pictq_cond,
				&global_context.pictq_mutex);
	}
	pthread_mutex_unlock(&global_context.pictq_mutex);

	// windex is set to 0 initially
	vp = &global_context.pictq[global_context.pictq_windex];
	if (vp->pFrame) {
		av_frame_unref(vp->pFrame);
		av_frame_free(&vp->pFrame);
	}
	vp->pFrame = pFrame;
	vp->width = global_context.vcodec_ctx->width;
	vp->height = global_context.vcodec_ctx->height;

	if (vp->pFrame) {
		vp->pts = pts;
		if (++global_context.pictq_windex >= VIDEO_PICTURE_QUEUE_SIZE) {
			global_context.pictq_windex = 0;
		}
		pthread_mutex_lock(&global_context.pictq_mutex);
		global_context.pictq_size++;
		pthread_mutex_unlock(&global_context.pictq_mutex);
	}

	return 0;
}

void video_display(AVFrame* pFrame) {
	renderSurface(pFrame);
}

void video_refresh_callback(unsigned int timer_id, void*) {
	video_refresh_timer();
}

void schedule_refresh(int delay) {
	timer_delay_ms = delay;
}

void video_refresh_timer() {
	VideoPicture *vp;
	double actual_delay, delay, sync_threshold, ref_clock, diff;

	if (global_context.pictq_size == 0) {
		schedule_refresh(1);
	} else {
		vp = &global_context.pictq[global_context.pictq_rindex];

		video_current_pts = vp->pts;
		global_context.video_current_pts_time = av_gettime();

		delay = vp->pts - global_context.frame_last_pts;

		if (delay <= 0 || delay >= 1.0) { // 非法值判断
			delay = global_context.frame_last_delay;
		}

		global_context.frame_last_delay = delay;
		global_context.frame_last_pts = vp->pts;

		ref_clock = get_master_clock();
		diff = vp->pts - ref_clock;

		sync_threshold =
				(delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
		if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
			if (diff <= -sync_threshold) {
				//av_log(NULL, AV_LOG_ERROR, "video_refresh_timer : skip. \n");
				LOGV("video_refresh_timer : skip. \n");
				delay = 0;
			} else if (diff >= sync_threshold) {
				//av_log(NULL, AV_LOG_ERROR, "video_refresh_timer : repeat. \n");
				LOGV("video_refresh_timer : repeat. \n");
				delay = 2 * delay;
			}
		} else {
			//av_log(NULL, AV_LOG_ERROR,
			//		" video_refresh_timer : diff > 10 , diff = %f, vp->pts = %f , ref_clock = %f\n",
			//		diff, vp->pts, ref_clock);
			LOGV(
					" video_refresh_timer : diff > 10 , diff = %f, vp->pts = %f , ref_clock = %f\n",
					diff, vp->pts, ref_clock);
		}

		global_context.frame_timer += delay;

		actual_delay = global_context.frame_timer - (av_gettime() / 1000000.0);
		if (actual_delay < 0.010) {    //每秒100帧的刷新率不存在

			actual_delay = 0.010;
		}
		schedule_refresh((int) (actual_delay * 1000 + 0.5)); //add 0.5 for 进位
		if (vp->pFrame)
			video_display(vp->pFrame);

		if (++global_context.pictq_rindex >= VIDEO_PICTURE_QUEUE_SIZE) {
			global_context.pictq_rindex = 0;
		}

		pthread_mutex_lock(&global_context.pictq_mutex);
		global_context.pictq_size--;
		pthread_cond_signal(&global_context.pictq_cond);
		pthread_mutex_unlock(&global_context.pictq_mutex);
	}
}

void* video_thread(void *argv) {
	AVPacket pkt1;
	AVPacket *packet = &pkt1;
	int frameFinished;

	double pts;

	for (;;) {

		if (global_context.quit) {
			av_log(NULL, AV_LOG_ERROR, "video_thread need exit. \n");
			break;
		}

		if (global_context.pause) {
			continue;
		}

		if (packet_queue_get(&global_context.video_queue, packet) <= 0) {
			// means we quit getting packets
			continue;
		}

		AVFrame *pFrame = av_frame_alloc();
		frameFinished = 0;
		avcodec_decode_video2(global_context.vcodec_ctx, pFrame, &frameFinished,
				packet);

		/*av_log(NULL, AV_LOG_ERROR,
		 "packet_queue_get size is %d, format is %d\n", packet->size,
		 pFrame->format);*/

		// Did we get a video frame?
		if (frameFinished) {
			pts = pFrame->pkt_pts * av_q2d(global_context.vstream->time_base);

			pts = synchronize_video(pFrame, pts);

			if (queue_picture(pFrame, pts) < 0) {
				break;
			}
		} else {
			av_frame_free(&pFrame);
		}

		av_packet_unref(packet);
		av_init_packet(packet);
	}

	return 0;
}

void* picture_thread(void *argv) {
	EGLBoolean success = eglMakeCurrent(global_context.eglDisplay,
			global_context.eglSurface, global_context.eglSurface,
			global_context.eglContext);
	CreateProgram();

	while (1) {
		if (global_context.quit) {
			break;
		}

		if (global_context.pause) {
			continue;
		}

		usleep(timer_delay_ms * 1000);

		video_refresh_timer();

	}
	return 0;
}

