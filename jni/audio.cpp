#include <assert.h>
#include <jni.h>
#include <string.h>
#include "player.h"

// for native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio
#define DECODE_AUDIO_BUFFER_SIZE ((AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) )
#define SAMPLE_CORRECTION_PERCENT_MAX 10
/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20
#define AV_NOSYNC_THRESHOLD 10.0

static AVFilterContext *in_audio_filter;  // the first filter in the audio chain
static AVFilterContext *out_audio_filter;  // the last filter in the audio chain
static AVFilterGraph *agraph;              // audio filter graph
static struct AudioParams audio_filter_src;

static double audio_clock;
static double last_enqueue_buffer_time;
static int last_enqueue_buffer_size;

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine;

// output mix interfaces
static SLObjectItf outputMixObject = NULL;
static SLEnvironmentalReverbItf outputMixEnvironmentalReverb = NULL;

// buffer queue player interfaces
static SLObjectItf bqPlayerObject = NULL;
static SLPlayItf bqPlayerPlay;
static SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
static SLEffectSendItf bqPlayerEffectSend;
static SLVolumeItf bqPlayerVolume;
static uint8_t decoded_audio_buf[AVCODEC_MAX_AUDIO_FRAME_SIZE];

double get_audio_clock() {
	double pts;
	int hw_buf_size, bytes_per_sec, n;

	pts = audio_clock;

	bytes_per_sec = 0;
	n = global_context.acodec_ctx->channels * 2;
	bytes_per_sec = global_context.acodec_ctx->sample_rate * n;
	hw_buf_size = last_enqueue_buffer_size
			- (double(av_gettime() - last_enqueue_buffer_time) / 1000000.0)
					* bytes_per_sec;
	if (bytes_per_sec) {
		pts -= (double) hw_buf_size / bytes_per_sec;
	}

	return pts;
}


static int init_filter_graph(AVFilterGraph **graph, AVFilterContext **src,
		AVFilterContext **sink) {
	AVFilterGraph *filter_graph;
	AVFilterContext *abuffer_ctx;
	AVFilter *abuffer;
	AVFilterContext *aformat_ctx;
	AVFilter *aformat;
	AVFilterContext *abuffersink_ctx;
	AVFilter *abuffersink;

	char options_str[1024];
	char ch_layout[64];

	int err;

	/* Create a new filtergraph, which will contain all the filters. */
	filter_graph = avfilter_graph_alloc();
	if (!filter_graph) {
		av_log(NULL, AV_LOG_ERROR, "Unable to create filter graph.\n");
		return AVERROR(ENOMEM);
	}

	/* Create the abuffer filter;
	 * it will be used for feeding the data into the graph. */
	abuffer = avfilter_get_by_name("abuffer");
	if (!abuffer) {
		av_log(NULL, AV_LOG_ERROR, "Could not find the abuffer filter.\n");
		return AVERROR_FILTER_NOT_FOUND ;
	}

	abuffer_ctx = avfilter_graph_alloc_filter(filter_graph, abuffer, "src");
	if (!abuffer_ctx) {
		av_log(NULL, AV_LOG_ERROR,
				"Could not allocate the abuffer instance.\n");
		return AVERROR(ENOMEM);
	}

	/* Set the filter options through the AVOptions API. */
	av_get_channel_layout_string(ch_layout, sizeof(ch_layout), (int) 0,
			audio_filter_src.channel_layout);
	av_opt_set(abuffer_ctx, "channel_layout", ch_layout,
			AV_OPT_SEARCH_CHILDREN);
	av_opt_set(abuffer_ctx, "sample_fmt",
			av_get_sample_fmt_name(audio_filter_src.fmt),
			AV_OPT_SEARCH_CHILDREN);
	av_opt_set_q(abuffer_ctx, "time_base",
			(AVRational ) { 1, audio_filter_src.freq },
			AV_OPT_SEARCH_CHILDREN);
	av_opt_set_int(abuffer_ctx, "sample_rate", audio_filter_src.freq,
			AV_OPT_SEARCH_CHILDREN);

	/* Now initialize the filter; we pass NULL options, since we have already
	 * set all the options above. */
	err = avfilter_init_str(abuffer_ctx, NULL);
	if (err < 0) {
		av_log(NULL, AV_LOG_ERROR,
				"Could not initialize the abuffer filter.\n");
		return err;
	}

	/* Create the aformat filter;
	 * it ensures that the output is of the format we want. */
	aformat = avfilter_get_by_name("aformat");
	if (!aformat) {
		av_log(NULL, AV_LOG_ERROR, "Could not find the aformat filter.\n");
		return AVERROR_FILTER_NOT_FOUND ;
	}

	aformat_ctx = avfilter_graph_alloc_filter(filter_graph, aformat, "aformat");
	if (!aformat_ctx) {
		av_log(NULL, AV_LOG_ERROR,
				"Could not allocate the aformat instance.\n");
		return AVERROR(ENOMEM);
	}

	/* A third way of passing the options is in a string of the form
	 * key1=value1:key2=value2.... */
	snprintf(options_str, sizeof(options_str),
			"sample_fmts=%s:sample_rates=%d:channel_layouts=0x%x",
			av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), audio_filter_src.freq,
			audio_filter_src.channel_layout);
	err = avfilter_init_str(aformat_ctx, options_str);
	if (err < 0) {
		av_log(NULL, AV_LOG_ERROR,
				"Could not initialize the aformat filter.\n");
		return err;
	}

	/* Finally create the abuffersink filter;
	 * it will be used to get the filtered data out of the graph. */
	abuffersink = avfilter_get_by_name("abuffersink");
	if (!abuffersink) {
		av_log(NULL, AV_LOG_ERROR, "Could not find the abuffersink filter.\n");
		return AVERROR_FILTER_NOT_FOUND ;
	}

	abuffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink,
			"sink");
	if (!abuffersink_ctx) {
		av_log(NULL, AV_LOG_ERROR,
				"Could not allocate the abuffersink instance.\n");
		return AVERROR(ENOMEM);
	}

	/* This filter takes no options. */
	err = avfilter_init_str(abuffersink_ctx, NULL);
	if (err < 0) {
		av_log(NULL, AV_LOG_ERROR,
				"Could not initialize the abuffersink instance.\n");
		return err;
	}

	/* Connect the filters;
	 * in this simple case the filters just form a linear chain. */
	err = avfilter_link(abuffer_ctx, 0, aformat_ctx, 0);
	if (err >= 0) {
		err = avfilter_link(aformat_ctx, 0, abuffersink_ctx, 0);
	}

	if (err < 0) {
		av_log(NULL, AV_LOG_ERROR, "Error connecting filters\n");
		return err;
	}

	/* Configure the graph. */
	err = avfilter_graph_config(filter_graph, NULL);
	if (err < 0) {
		av_log(NULL, AV_LOG_ERROR, "Error configuring the filter graph\n");
		return err;
	}

	*graph = filter_graph;
	*src = abuffer_ctx;
	*sink = abuffersink_ctx;

	return 0;
}

static inline int64_t get_valid_channel_layout(int64_t channel_layout,
		int channels) {
	if (channel_layout
			&& av_get_channel_layout_nb_channels(channel_layout) == channels) {
		return channel_layout;
	} else {
		return 0;
	}
}

// decode a new packet(not multi-frame)
// return decoded frame size, not decoded packet size
int audio_decode_frame(uint8_t *audio_buf, int buf_size) {
	static AVPacket pkt;
	static uint8_t *audio_pkt_data = NULL;
	static int audio_pkt_size = 0;
	int len1, data_size;
	int got_frame;
	AVFrame * frame = NULL;
	static int reconfigure = 1;
	int ret = -1;

	for (;;) {

		while (audio_pkt_size > 0) {

			if (NULL == frame) {
				frame = av_frame_alloc();
			}

			data_size = buf_size;
			got_frame = 0;

			// len1 is decoded packet size
			len1 = avcodec_decode_audio4(global_context.acodec_ctx, frame,
					&got_frame, &pkt);
			if (got_frame) {

				if (reconfigure) {

					reconfigure = 0;
					int64_t dec_channel_layout = get_valid_channel_layout(
							frame->channel_layout,
							av_frame_get_channels(frame));

					// used by init_filter_graph()
					audio_filter_src.fmt = (enum AVSampleFormat) frame->format;
					audio_filter_src.channels = av_frame_get_channels(frame);
					audio_filter_src.channel_layout = dec_channel_layout;
					audio_filter_src.freq = frame->sample_rate;

					init_filter_graph(&agraph, &in_audio_filter,
							&out_audio_filter);
				}

				if ((ret = av_buffersrc_add_frame(in_audio_filter, frame))
						< 0) {
					av_log(NULL, AV_LOG_ERROR,
							"av_buffersrc_add_frame :  failure. \n");
					return ret;
				}

				if ((ret = av_buffersink_get_frame(out_audio_filter, frame))
						< 0) {
					av_log(NULL, AV_LOG_ERROR,
							"av_buffersink_get_frame :  failure. \n");
					continue;
				}

				data_size = av_samples_get_buffer_size(NULL, frame->channels,
						frame->nb_samples, (enum AVSampleFormat) frame->format,
						1);

				// len1 is decoded packet size
				// < 0  means failure or error，so break to get a new packet
				if (len1 < 0) {
					audio_pkt_size = 0;
					av_log(NULL, AV_LOG_ERROR,
							"avcodec_decode_audio4 failure. \n");
					break;
				}

				// decode ok, sync audio, just give synced size
				//synchronize_audio((int16_t *)audio_buf, data_size);

				// decoded data to audio buf
				memcpy(audio_buf, frame->data[0], data_size);

				audio_pkt_data += len1;
				audio_pkt_size -= len1;

				int n = 2 * global_context.acodec_ctx->channels;
				audio_clock += (double) data_size
						/ (double) (n * global_context.acodec_ctx->sample_rate); // add bytes offset
				//LOGV2("audio_decode_frame: 2 pts is %lld, %lf", pkt.pts, audio_clock);
				av_free_packet(&pkt);
				av_frame_free(&frame);

				return data_size;
			} else if (len1 < 0) {
				char errbuf[64];
				av_strerror(ret, errbuf, 64);
				LOGV2("avcodec_decode_audio4 ret < 0, %s", errbuf);
			}
		}

		av_free_packet(&pkt);
		av_frame_free(&frame);

		// get a new packet
		if (packet_queue_get(&global_context.audio_queue, &pkt) < 0) {
			return -1;
		}

		//LOGV2("pkt.size is %d", pkt.size);

		audio_pkt_data = pkt.data;
		audio_pkt_size = pkt.size;

		// save current pts clock
		if (pkt.pts != AV_NOPTS_VALUE ) {
			audio_clock = pkt.pts * av_q2d(global_context.astream->time_base);
			//LOGV2("audio_decode_frame: 1 pts is %lld, %lf", pkt.pts, audio_clock);
		}
	}

	return ret;
}

// this callback handler is called every time a buffer finishes playing
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context) {
	SLresult result;

	//LOGV2("bqPlayerCallback...");

	if (bq != bqPlayerBufferQueue) {
		LOGV2("bqPlayerCallback : not the same player object.");
		return;
	}

	int decoded_size = audio_decode_frame(decoded_audio_buf,
			sizeof(decoded_audio_buf));
	if (decoded_size > 0) {
		result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue,
				decoded_audio_buf, decoded_size);
		last_enqueue_buffer_time = av_gettime();
		last_enqueue_buffer_size = decoded_size;
		// the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
		// which for this code example would indicate a programming error
		if (SL_RESULT_SUCCESS != result) {
			LOGV2("bqPlayerCallback : bqPlayerBufferQueue Enqueue failure.");
		}
	}
}

int createEngine() {

	SLresult result;

	// create engine
	result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("slCreateEngine failure.");
		return -1;
	}

	// realize the engine
	result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE );
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("engineObject Realize failure.");
		(*engineObject)->Destroy(engineObject);
		engineObject = NULL;
		engineEngine = NULL;
		return -1;
	}

	// get the engine interface, which is needed in order to create other objects
	result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE,
			&engineEngine);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("engineObject GetInterface failure.");
		(*engineObject)->Destroy(engineObject);
		engineObject = NULL;
		engineEngine = NULL;
		return -1;
	}

	// create output mix, with environmental reverb specified as a non-required interface
	const SLInterfaceID ids[1] = { SL_IID_ENVIRONMENTALREVERB };
	const SLboolean req[1] = { SL_BOOLEAN_FALSE };
	result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 1,
			ids, req);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("engineObject CreateOutputMix failure.");
		(*engineObject)->Destroy(engineObject);
		engineObject = NULL;
		engineEngine = NULL;
		return -1;
	}

	// realize the output mix
	result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE );
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("outputMixObject Realize failure.");

		(*outputMixObject)->Destroy(outputMixObject);
		outputMixObject = NULL;
		outputMixEnvironmentalReverb = NULL;
		(*engineObject)->Destroy(engineObject);
		engineObject = NULL;
		engineEngine = NULL;
		return -1;
	}

	// get the environmental reverb interface
	// this could fail if the environmental reverb effect is not available,
	// either because the feature is not present, excessive CPU load, or
	// the required MODIFY_AUDIO_SETTINGS permission was not requested and granted
	result = (*outputMixObject)->GetInterface(outputMixObject,
			SL_IID_ENVIRONMENTALREVERB, &outputMixEnvironmentalReverb);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("outputMixObject Realize failure.");
		(*outputMixObject)->Destroy(outputMixObject);
		outputMixObject = NULL;
		outputMixEnvironmentalReverb = NULL;
		(*engineObject)->Destroy(engineObject);
		engineObject = NULL;
		engineEngine = NULL;
		return -1;
	}

	LOGV2("OpenSL ES createEngine success.");
	return 0;
}

int createBufferQueueAudioPlayer() {
	SLresult result;
	SLuint32 channelMask;

	// configure audio source
	SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
			SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2 };

	if (global_context.acodec_ctx->channels == 2)
		channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
	else
		channelMask = SL_SPEAKER_FRONT_CENTER;

	SLDataFormat_PCM format_pcm = { SL_DATAFORMAT_PCM,
			global_context.acodec_ctx->channels,
			global_context.acodec_ctx->sample_rate * 1000,
			SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
			channelMask, SL_BYTEORDER_LITTLEENDIAN };

	SLDataSource audioSrc = { &loc_bufq, &format_pcm };

	// configure audio sink
	SLDataLocator_OutputMix loc_outmix = { SL_DATALOCATOR_OUTPUTMIX,
			outputMixObject };
	SLDataSink audioSnk = { &loc_outmix, NULL };

	// create audio player
	const SLInterfaceID ids[3] = { SL_IID_BUFFERQUEUE, SL_IID_EFFECTSEND,
			SL_IID_VOLUME };
	const SLboolean req[3] =
			{ SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
	result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject,
			&audioSrc, &audioSnk, 3, ids, req);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("CreateAudioPlayer failure.");
		return -1;
	}

	// realize the player
	result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE );
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("bqPlayerObject Realize failure.");
		return -1;
	}

	// get the play interface
	result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY,
			&bqPlayerPlay);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("bqPlayerObject GetInterface failure.");
		return -1;
	}

	// get the buffer queue interface
	result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
			&bqPlayerBufferQueue);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("bqPlayerObject GetInterface failure.");
		return -1;
	}

	// register callback on the buffer queue
	result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue,
			bqPlayerCallback, NULL);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("bqPlayerObject RegisterCallback failure.");
		return -1;
	}

	// get the effect send interface
	result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_EFFECTSEND,
			&bqPlayerEffectSend);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("bqPlayerObject GetInterface SL_IID_EFFECTSEND failure.");
		return -1;
	}

	// get the volume interface
	result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME,
			&bqPlayerVolume);
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("bqPlayerObject GetInterface SL_IID_VOLUME failure.");
		return -1;
	}

	// set the player's state to playing
	result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING );
	if (SL_RESULT_SUCCESS != result) {
		LOGV2("bqPlayerObject SetPlayState SL_PLAYSTATE_PLAYING failure.");
		return -1;
	}

	LOGV2("OpenSL ES CreateAudioPlayer success.");

	return 0;
}

void fireOnPlayer() {
	bqPlayerCallback(bqPlayerBufferQueue, NULL);
}

/**
 * Destroys the given object instance.
 *
 * @param object object instance. [IN/OUT]
 */
static void DestroyObject(SLObjectItf& object) {
	if (0 != object)
		(*object)->Destroy(object);

	object = 0;
}

void destroyPlayerAndEngine() {
	(*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED );

	// Destroy audio player object
	DestroyObject(bqPlayerObject);

	// Destroy output mix object
	DestroyObject(outputMixObject);

	// Destroy the engine instance
	DestroyObject(engineObject);
}

