#include <stdio.h>
#include <limits.h>
#include <os.h>
#include <common.h>
#include <ring_buffer.h>
#include <driver/systick.h>
#include "jz_audio_info.h"
#include "audio_core_service.h"
#include "audio_dev.h"
#include "audio_run_status_ctrl.h"
#include <dfs.h>
#include <dfs_fs.h>
#include <dfs_file.h>
#include <dfs_posix.h>
#include "audio_cap.h"
#include "vad_point.h"
#include "time_statistics.h"
#include "volume_setting.h"
#include "voxa.h"
#include "audio_3A_ini_config.h"

#define AEC_CAP_DELAY_MS 60
#define AEC_CAP_DELAY_FIFO_PACKETS 8

#define AUDIO_LITE_FRAME_SAMPLES 160U
#define AUDIO_LITE_NS_POLICY 2
#define AUDIO_LITE_AEC_DELAY_MS 60
#define AUDIO_LITE_STATS_PERIOD_PACKETS 250U

typedef struct NsHandleT NsHandle;

extern void *WebRtcAec_Create(void);
extern void WebRtcAec_Free(void *aec_inst);
extern int WebRtcAec_Init(void *aec_inst, int32_t sample_rate,
    int32_t soundcard_sample_rate);
extern int WebRtcAec_BufferFarend(void *aec_inst, const float *farend,
    size_t samples);
extern int WebRtcAec_Process(void *aec_inst,
    const float *const *nearend, size_t bands, float *const *output,
    size_t samples, int16_t delay_ms, int32_t skew);
extern NsHandle *WebRtcNs_Create(void);
extern void WebRtcNs_Free(NsHandle *ns_inst);
extern int WebRtcNs_Init(NsHandle *ns_inst, uint32_t sample_rate);
extern int WebRtcNs_set_policy(NsHandle *ns_inst, int mode);
extern void WebRtcNs_Analyze(NsHandle *ns_inst, const float *speech_frame);
extern void WebRtcNs_Process(NsHandle *ns_inst,
    const float *const *speech_frame, size_t bands, float *const *output);

typedef struct {
    void *aec;
    NsHandle *ns;
    float render[AUDIO_LITE_FRAME_SAMPLES];
    float capture[AUDIO_LITE_FRAME_SAMPLES];
    float aec_output[AUDIO_LITE_FRAME_SAMPLES];
    float ns_output[AUDIO_LITE_FRAME_SAMPLES];
    unsigned int packet_count;
    unsigned int aec_fail_count;
    bool aec_ready;
    bool ns_ready;
} audio_lite_processor_t;

static struct pcm_device *c_codec = NULL;
static struct pcm_device *c_dai = NULL;
static struct pcm_device *l_dai = NULL;
static volatile audio_run_status_t audio_cap_status = AUDIO_RUN_INIT;
static volatile int audio_cap_3a_enabled = 1;

typedef struct {
    int module;
    int cap;
    int echo_cancellation;
    int denoise;
    int cap_only;
    int loop_only;
} jz_audio_cap_time_stats;

static jz_audio_cap_time_stats audio_cap_time_stats = {0};

static unsigned int audio_abs_s16(int16_t sample)
{
    if (sample == INT16_MIN)
        return (unsigned int)INT16_MAX + 1U;

    return sample < 0 ? (unsigned int)-sample : (unsigned int)sample;
}

static int16_t audio_float_to_s16(float sample)
{
    if (sample > (float)INT16_MAX)
        return INT16_MAX;
    if (sample < (float)INT16_MIN)
        return INT16_MIN;

    return (int16_t)sample;
}

static void audio_lite_processor_deinit(audio_lite_processor_t *processor)
{
    if (!processor)
        return;

    if (processor->aec) {
        WebRtcAec_Free(processor->aec);
        processor->aec = NULL;
    }
    if (processor->ns) {
        WebRtcNs_Free(processor->ns);
        processor->ns = NULL;
    }
    processor->aec_ready = false;
    processor->ns_ready = false;
}

static int audio_lite_processor_init(audio_lite_processor_t *processor)
{
    if (!processor)
        return -EINVAL;

    memset(processor, 0, sizeof(*processor));
    processor->aec = WebRtcAec_Create();
    if (processor->aec &&
        WebRtcAec_Init(processor->aec, 16000, 16000) == 0) {
        processor->aec_ready = true;
    } else if (processor->aec) {
        WebRtcAec_Free(processor->aec);
        processor->aec = NULL;
    }

    processor->ns = WebRtcNs_Create();
    if (processor->ns && WebRtcNs_Init(processor->ns, 16000U) == 0 &&
        WebRtcNs_set_policy(processor->ns, AUDIO_LITE_NS_POLICY) == 0) {
        processor->ns_ready = true;
    } else if (processor->ns) {
        WebRtcNs_Free(processor->ns);
        processor->ns = NULL;
    }

    jz_log_dump(JZ_APP_COM_MOD,
        "audio lite WebRTC ready aec=%d ns=%d rate=16000 frame=10ms policy=%d\n",
        processor->aec_ready ? 1 : 0, processor->ns_ready ? 1 : 0,
        AUDIO_LITE_NS_POLICY);
    return processor->aec_ready || processor->ns_ready ? 0 : -ENOMEM;
}

static void audio_lite_processor_process(audio_lite_processor_t *processor,
    const int16_t *capture, const int16_t *render, int16_t *output,
    unsigned int samples)
{
    unsigned int raw_peak = 0;
    unsigned int output_peak = 0;
    unsigned int raw_sum = 0;
    unsigned int output_sum = 0;
    unsigned int clipped = 0;

    if (!processor || !capture || !output ||
        samples == 0U || samples % AUDIO_LITE_FRAME_SAMPLES != 0U) {
        if (capture && output && samples)
            memcpy(output, capture, samples * sizeof(*output));
        return;
    }

    for (unsigned int offset = 0; offset < samples;
         offset += AUDIO_LITE_FRAME_SAMPLES) {
        const float *near_bands[1];
        const float *ns_bands[1];
        float *aec_output_bands[1];
        float *ns_output_bands[1];
        const float *stage;
        int aec_result = -1;

        for (unsigned int i = 0; i < AUDIO_LITE_FRAME_SAMPLES; ++i) {
            processor->capture[i] = (float)capture[offset + i];
            processor->render[i] = render ? (float)render[offset + i] : 0.0f;
        }

        stage = processor->capture;
        if (processor->aec_ready) {
            near_bands[0] = processor->capture;
            aec_output_bands[0] = processor->aec_output;
            aec_result = WebRtcAec_BufferFarend(processor->aec,
                processor->render, AUDIO_LITE_FRAME_SAMPLES);
            if (aec_result == 0) {
                aec_result = WebRtcAec_Process(processor->aec,
                    near_bands, 1U, aec_output_bands,
                    AUDIO_LITE_FRAME_SAMPLES, AUDIO_LITE_AEC_DELAY_MS, 0);
            }
            if (aec_result == 0) {
                stage = processor->aec_output;
            } else {
                processor->aec_fail_count++;
            }
        }

        if (processor->ns_ready) {
            ns_bands[0] = stage;
            ns_output_bands[0] = processor->ns_output;
            WebRtcNs_Analyze(processor->ns, stage);
            WebRtcNs_Process(processor->ns, ns_bands, 1U, ns_output_bands);
            stage = processor->ns_output;
        }

        for (unsigned int i = 0; i < AUDIO_LITE_FRAME_SAMPLES; ++i)
            output[offset + i] = audio_float_to_s16(stage[i]);
    }

    processor->packet_count++;
    if (processor->packet_count == 1U ||
        processor->packet_count % AUDIO_LITE_STATS_PERIOD_PACKETS == 0U) {
        for (unsigned int i = 0; i < samples; ++i) {
            unsigned int raw_level = audio_abs_s16(capture[i]);
            unsigned int output_level = audio_abs_s16(output[i]);

            raw_sum += raw_level;
            output_sum += output_level;
            if (raw_level > raw_peak)
                raw_peak = raw_level;
            if (output_level > output_peak)
                output_peak = output_level;
            if (raw_level >= 32000U)
                clipped++;
        }
        jz_log_dump(JZ_APP_COM_MOD,
            "audio lite packet=%u aec=%d ns=%d raw_peak=%u raw_mean=%u out_peak=%u out_mean=%u clipped=%u aec_fail=%u\n",
            processor->packet_count, processor->aec_ready ? 1 : 0,
            processor->ns_ready ? 1 : 0, raw_peak, raw_sum / samples,
            output_peak, output_sum / samples, clipped,
            processor->aec_fail_count);
    }
}

static voxa_handle_t *audio_cap_3A_create(const char *ini_path)
{
    voxa_handle_t *handle = NULL;
    audio_3A_ini_config_t ini_config;

    handle = voxa_create();
    if (!handle) {
        jz_log_error(JZ_APP_COM_MOD, "voxa_create failed\n");
        return NULL;
    }

    if (audio_3A_ini_load_file(ini_path, handle, VOXA_PIPELINE_DUPLEX,
            &ini_config) != 0 ||
        audio_3A_ini_apply_to_handle(handle, VOXA_PIPELINE_DUPLEX,
            &ini_config) != 0 ||
        voxa_init(handle) != VOXA_OK) {
        jz_log_error(JZ_APP_COM_MOD, "voxa init failed, ini:%s\n", ini_path);
        voxa_destroy(handle);
        return NULL;
    }

    return handle;
}
static unsigned int audio_max_u32(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

static bool audio_should_log_backoff(unsigned int count)
{
    return count && ((count & (count - 1)) == 0);
}

static unsigned int audio_div_round_up(unsigned int x, unsigned int y)
{
    if (!y)
        return 0;

    return (x + y - 1) / y;
}

static unsigned int audio_ms_to_bytes(struct pcm_params *params, unsigned int ms)
{
    return pcm_data_sample_rate(params->pcm_sample_rate) *
           ms / 1000 *
           pcm_frame_size(params);
}

static unsigned int audio_bytes_to_ms(struct pcm_params *params, unsigned int bytes)
{
    unsigned int byte_rate = pcm_data_sample_rate(params->pcm_sample_rate) *
                             pcm_frame_size(params);

    if (!byte_rate)
        return 0;

    return bytes * 1000 / byte_rate;
}

typedef struct {
    char *buffer;
    unsigned int packet_size;
    unsigned int packet_count;
    unsigned int read_idx;
    unsigned int write_idx;
    unsigned int cached_packets;
} audio_cap_delay_fifo_t;

static int audio_cap_delay_fifo_init(audio_cap_delay_fifo_t *fifo,
    unsigned int packet_size, unsigned int packet_count)
{
    if (!fifo || !packet_size || !packet_count)
        return -EINVAL;

    memset(fifo, 0, sizeof(*fifo));
    fifo->buffer = malloc(packet_size * packet_count);
    if (!fifo->buffer)
        return -ENOMEM;

    fifo->packet_size = packet_size;
    fifo->packet_count = packet_count;

    return 0;
}

static void audio_cap_delay_fifo_deinit(audio_cap_delay_fifo_t *fifo)
{
    if (!fifo)
        return;

    if (fifo->buffer) {
        free(fifo->buffer);
        fifo->buffer = NULL;
    }

    memset(fifo, 0, sizeof(*fifo));
}

static int audio_cap_delay_fifo_push(audio_cap_delay_fifo_t *fifo, const void *data)
{
    char *dst;

    if (!fifo || !fifo->buffer || !data)
        return -EINVAL;

    if (fifo->cached_packets >= fifo->packet_count)
        return -ENOSPC;

    dst = fifo->buffer + fifo->write_idx * fifo->packet_size;
    memcpy(dst, data, fifo->packet_size);

    fifo->write_idx = (fifo->write_idx + 1) % fifo->packet_count;
    fifo->cached_packets++;

    return 0;
}

static int audio_cap_delay_fifo_pop(audio_cap_delay_fifo_t *fifo, void *data)
{
    char *src;

    if (!fifo || !fifo->buffer || !data)
        return -EINVAL;

    if (!fifo->cached_packets)
        return -ENOENT;

    src = fifo->buffer + fifo->read_idx * fifo->packet_size;
    memcpy(data, src, fifo->packet_size);

    fifo->read_idx = (fifo->read_idx + 1) % fifo->packet_count;
    fifo->cached_packets--;

    return 0;
}

static int audio_cap_delay_fifo_drop_oldest(audio_cap_delay_fifo_t *fifo)
{
    if (!fifo || !fifo->buffer)
        return -EINVAL;

    if (!fifo->cached_packets)
        return -ENOENT;

    fifo->read_idx = (fifo->read_idx + 1) % fifo->packet_count;
    fifo->cached_packets--;

    return 0;
}

static void audio_cap_log_read_status(const char *tag, int frame_read,
    unsigned int *fail_count)
{
    if (frame_read > 0) {
        if (fail_count && *fail_count) {
            jz_log_dump(JZ_APP_COM_MOD, "%s recovered after %u fails, frame_read=%d\n",
                tag, *fail_count, frame_read);
            *fail_count = 0;
        }
        return;
    }

    if (!fail_count)
        return;

    (*fail_count)++;
    if (audio_should_log_backoff(*fail_count)) {
        jz_log_dump(JZ_APP_COM_MOD, "%s fail: frame_read=%d count=%u\n",
            tag, frame_read, *fail_count);
    }
}

static void jz_audio_cap_time_stats_init(void)
{
    audio_cap_time_stats.module = jz_time_stats_register_module("audio_cap");
    audio_cap_time_stats.cap = jz_time_stats_add_stage(audio_cap_time_stats.module, "cap");
    audio_cap_time_stats.cap_only = jz_time_stats_add_stage(audio_cap_time_stats.module, "cap_only");
    audio_cap_time_stats.loop_only = jz_time_stats_add_stage(audio_cap_time_stats.module, "loop_only");
    audio_cap_time_stats.echo_cancellation = jz_time_stats_add_stage(audio_cap_time_stats.module, "echo");
    audio_cap_time_stats.denoise = jz_time_stats_add_stage(audio_cap_time_stats.module, "denoise");
    jz_time_stats_set_stage_thresholds(audio_cap_time_stats.cap, 5, 20);
    jz_time_stats_set_stage_thresholds(audio_cap_time_stats.echo_cancellation, 5, 12);
    jz_time_stats_set_stage_thresholds(audio_cap_time_stats.denoise, 4, 20);
}

static void audio_cap_controller_init(void)
{
    jz_audio_config_t *audio_config = jz_get_audio_params();

    c_codec = pcm_get("icodec-capture");
    if (c_codec == NULL) {
        jz_log_error(JZ_APP_COM_MOD, "c_codec is NULL\n");
        return;
    }

    c_dai = pcm_get("aic-capture");
    if (c_dai == NULL) {
        jz_log_error(JZ_APP_COM_MOD, "c_dai is NULL\n");
        return;
    }

    l_dai = pcm_get("aic-loopback");
    if (l_dai == NULL) {
        jz_log_error(JZ_APP_COM_MOD, "l_dai is NULL\n");
        return;
    }

    pcm_enable(c_codec, &(audio_config->params));
    thread_waiter_wakeup(jz_get_start_codec_waiter());
    pcm_enable(l_dai, &(audio_config->params));
    pcm_enable(c_dai, &(audio_config->params));
    pcm_start(c_codec);
    pcm_start(l_dai);
    pcm_start(c_dai);
}

static void audio_cap_controller_deinit(void)
{
    pcm_stop(l_dai);
    pcm_stop(c_dai);
    thread_waiter_wait(jz_get_stop_codec_waiter());
    pcm_stop(c_codec);
    pcm_disable(c_dai);
    pcm_disable(l_dai);
    pcm_disable(c_codec);

    c_codec = NULL;
    c_dai = NULL;
    l_dai = NULL;
}

static void audio_cap_thread(void *arg)
{
    voxa_handle_t* handle  = NULL;
    audio_lite_processor_t lite_processor = {0};
    voxa_metrics_t metrics;
    jz_audio_config_t *audio_config = jz_get_audio_params();
    char *audio_buffer = NULL;
    char *audio_loop_buffer = NULL;
    char *audio_out_buffer = NULL;
    audio_frame_header_t frame_header = {0};
    bool controller_inited = false;
    int ret;
    unsigned int loop_read_fail_count = 0;
    unsigned int cap_read_fail_count = 0;
    int loop_frame_read = 0;
    int cap_frame_read = 0;
    bool use_3a;

    (void)arg;

    audio_cap_delay_fifo_t cap_delay_fifo = {0};
    unsigned int cap_delay_packets = 0;
    unsigned int cap_delay_fifo_packets = 0;
    unsigned int cap_delay_fill_count = 0;
    unsigned int cap_delay_overrun_count = 0;
    unsigned int cap_delay_bytes = 0;
    bool cap_delay_armed = false;
    bool cap_delay_first_packet_logged = false;

    /* time * channels * data_fmt * sample_rate */
    int pack_size = jz_get_audio_pack_size();
    audio_buffer = (char *)malloc(pack_size * 3);
    if (audio_buffer == NULL) {
        jz_log_error(JZ_APP_COM_MOD, "failed to get audio_buffer\n");
        goto out;
    }
    audio_loop_buffer = audio_buffer + pack_size;
    audio_out_buffer = audio_buffer + 2 * pack_size;
    memset(audio_buffer, 0, pack_size * 3);
    int frame_size = pack_size / pcm_frame_size(&(audio_config->params));
    printf("[%s:%d] pack_size:%d packet_num:%d frame_size:%d\n", __FUNCTION__, __LINE__, pack_size, frame_size, pcm_frame_size(&(audio_config->params)));


    audio_cap_controller_init();
    if (!c_codec || !c_dai || !l_dai)
        goto out;
    controller_inited = true;

    jz_set_volume_capture_by_json();

    jz_log_debug(JZ_APP_COM_MOD, "cap dai name:%s\n", c_dai->data->name);
    jz_log_debug(JZ_APP_COM_MOD, "in codec name:%s\n", c_codec->data->name);
    jz_log_debug(JZ_APP_COM_MOD, "channels:%d\n", audio_config->params.channels);
    jz_log_debug(JZ_APP_COM_MOD, "pcm_data_fmt:%d\n", audio_config->params.pcm_data_fmt);
    jz_log_debug(JZ_APP_COM_MOD, "pcm_sample_rate:%d\n", audio_config->params.pcm_sample_rate);
    jz_log_debug(JZ_APP_COM_MOD, "pcm_interface:%d\n", audio_config->params.pcm_interface);
    jz_log_debug(JZ_APP_COM_MOD, "i2s_frame_mode:%d\n", audio_config->params.i2s_frame_mode);
    jz_log_debug(JZ_APP_COM_MOD, "i2s_bclk_direction:%d\n", audio_config->params.i2s_bclk_direction);
    jz_log_debug(JZ_APP_COM_MOD, "i2s_frame_direction:%d\n", audio_config->params.i2s_frame_direction);

    use_3a = audio_cap_3a_enabled != 0;
    jz_log_dump(JZ_APP_COM_MOD, "audio capture mode 3a:%d\n", use_3a ? 1 : 0);
    if (use_3a) {
        handle = audio_cap_3A_create("/fs/audio/config/voxa.ini");
        if (!handle) {
            use_3a = false;
            jz_log_dump(JZ_APP_COM_MOD,
                "VOXA unavailable, using raw microphone capture\n");
        }
    } else {
        jz_log_dump(JZ_APP_COM_MOD,
            "VOXA disabled, using low-memory WebRTC audio processor\n");
    }

    thread_waiter_wait(jz_get_pwma_start_waiter());

    if (!use_3a) {
        ret = audio_lite_processor_init(&lite_processor);
        if (ret) {
            jz_log_error(JZ_APP_COM_MOD,
                "audio lite WebRTC init failed:%d, raw capture fallback remains active\n",
                ret);
        }
    }

    if (use_3a) {
        cap_delay_bytes = audio_ms_to_bytes(&(audio_config->params), AEC_CAP_DELAY_MS);
        cap_delay_packets = audio_div_round_up(cap_delay_bytes,
            (unsigned int)pack_size);
        cap_delay_fifo_packets = audio_max_u32(cap_delay_packets + 4,
            AEC_CAP_DELAY_FIFO_PACKETS);

        ret = audio_cap_delay_fifo_init(&cap_delay_fifo, pack_size,
            cap_delay_fifo_packets);
        if (ret) {
            jz_log_error(JZ_APP_COM_MOD,
                "audio cap delay fifo init fail:%d\n", ret);
            goto out;
        }
    }

    #if 0
    ret = pcm_private_ctrl(c_dai, "cap_frame_hard_sync", 0);
    if (ret)
        jz_log_dump(JZ_APP_COM_MOD, "audio cap hard sync fail:%d\n", ret);

    ret = pcm_private_ctrl(l_dai, "loop_frame_hard_sync", 0);
    if (ret)
        jz_log_dump(JZ_APP_COM_MOD, "audio loop hard sync fail:%d\n", ret);
    #endif

    if (use_3a) {
        jz_log_dump(JZ_APP_COM_MOD,
            "audio cap delay init: req=%ums actual=%uB(%ums) delay_pkts=%u fifo_pkts=%u pack=%dB\n",
            AEC_CAP_DELAY_MS,
            cap_delay_packets * (unsigned int)pack_size,
            audio_bytes_to_ms(&(audio_config->params),
                cap_delay_packets * (unsigned int)pack_size),
            cap_delay_packets, cap_delay_fifo_packets, pack_size);
    }

    if (!jz_audio_run_status_set(&audio_cap_status, AUDIO_RUN_RUNNING, "audio_cap")) {
        goto out;
    }

    while (audio_cap_status == AUDIO_RUN_RUNNING) {
        jz_time_stats_start(audio_cap_time_stats.cap);
        //两次读取间隔需要在pack_size对应时间内，如20ms
        jz_time_stats_start(audio_cap_time_stats.cap_only);
        cap_frame_read = audio_dev_read_data(c_dai, audio_buffer, pack_size);
        audio_cap_log_read_status("audio cap read", cap_frame_read, &cap_read_fail_count);
        jz_time_stats_end(audio_cap_time_stats.cap_only);

        if (!use_3a) {
            if (cap_frame_read <= 0)
                continue;

            if (lite_processor.aec_ready) {
                jz_time_stats_start(audio_cap_time_stats.loop_only);
                loop_frame_read = audio_dev_read_data(l_dai, audio_loop_buffer,
                    pack_size);
                audio_cap_log_read_status("audio loop read", loop_frame_read,
                    &loop_read_fail_count);
                jz_time_stats_end(audio_cap_time_stats.loop_only);
                if (loop_frame_read <= 0)
                    memset(audio_loop_buffer, 0, pack_size);
            } else {
                memset(audio_loop_buffer, 0, pack_size);
            }

            audio_lite_processor_process(&lite_processor,
                (const int16_t *)audio_buffer,
                (const int16_t *)audio_loop_buffer,
                (int16_t *)audio_out_buffer,
                (unsigned int)pack_size / sizeof(int16_t));
            jz_time_stats_end(audio_cap_time_stats.cap);
            goto distribute_frame;
        }

        jz_time_stats_start(audio_cap_time_stats.loop_only);
        loop_frame_read = audio_dev_read_data(l_dai, audio_loop_buffer, pack_size);
        audio_cap_log_read_status("audio loop read", loop_frame_read,
            &loop_read_fail_count);
        jz_time_stats_end(audio_cap_time_stats.loop_only);

        if (cap_frame_read > 0) {
            ret = audio_cap_delay_fifo_push(&cap_delay_fifo, audio_buffer);
            if (ret == -ENOSPC) {
                audio_cap_delay_fifo_drop_oldest(&cap_delay_fifo);
                cap_delay_overrun_count++;
                if (audio_should_log_backoff(cap_delay_overrun_count)) {
                    jz_log_dump(JZ_APP_COM_MOD,
                        "audio cap delay overflow: drop oldest count=%u cached=%u\n",
                        cap_delay_overrun_count, cap_delay_fifo.cached_packets);
                }
                ret = audio_cap_delay_fifo_push(&cap_delay_fifo, audio_buffer);
            }

            if (ret) {
                jz_log_error(JZ_APP_COM_MOD, "audio cap delay push fail:%d\n", ret);
                continue;
            }
        }

        if (!cap_delay_armed) {
            if (cap_delay_fifo.cached_packets <= cap_delay_packets) {
                cap_delay_fill_count++;
                if (audio_should_log_backoff(cap_delay_fill_count)) {
                    jz_log_dump(JZ_APP_COM_MOD,
                        "audio cap delay filling: cached=%u/%u delay_pkts=%u loop_read=%d cap_read=%d\n",
                        cap_delay_fifo.cached_packets, cap_delay_packets + 1,
                        cap_delay_packets, loop_frame_read, cap_frame_read);
                }
            } else {
                cap_delay_armed = true;
                jz_log_dump(JZ_APP_COM_MOD,
                    "audio cap delay armed: cached=%u delay=%uB(%ums) loop_read=%d cap_read=%d\n",
                    cap_delay_fifo.cached_packets,
                    cap_delay_packets * (unsigned int)pack_size,
                    audio_bytes_to_ms(&(audio_config->params),
                        cap_delay_packets * (unsigned int)pack_size),
                    loop_frame_read, cap_frame_read);
            }
        }

        if (loop_frame_read > 0 && cap_frame_read > 0 &&
            cap_delay_armed && !cap_delay_first_packet_logged) {
            jz_log_dump(JZ_APP_COM_MOD,
                "audio cap delay first packet: loop_read=%d cap_read=%d cached=%u pack=%dB\n",
                loop_frame_read, cap_frame_read, cap_delay_fifo.cached_packets, pack_size);
            cap_delay_first_packet_logged = true;
        }

        jz_time_stats_end(audio_cap_time_stats.cap);

        if (loop_frame_read <= 0 || cap_frame_read <= 0)
            continue;

        if (!cap_delay_armed)
            continue;

        ret = audio_cap_delay_fifo_pop(&cap_delay_fifo, audio_buffer);
        if (ret) {
            jz_log_error(JZ_APP_COM_MOD, "audio cap delay pop fail:%d\n", ret);
            continue;
        }

        audio_out_buffer = audio_buffer + 2 * pack_size;

        for (int i = 0; i < 2; i++) {
            int16_t *loop_frame = (int16_t *)audio_loop_buffer + i * 160;
            int16_t *cap_frame = (int16_t *)audio_buffer + i * 160;
            int16_t *out_frame = (int16_t *)audio_out_buffer + i * 160;
            voxa_duplex_frame_t voxa_frame = {
                .render_data = loop_frame,
                .capture_data = cap_frame,
                .output_data = out_frame,
                .samples_per_channel = 160,
            };

            jz_time_stats_start(audio_cap_time_stats.echo_cancellation);
            ret = voxa_process_duplex(handle, &voxa_frame);
            if (ret != VOXA_OK) {
                jz_log_error(JZ_APP_COM_MOD, "ERROR: voxa_process_duplex = %d\n", ret);
                goto out;
            }

            /* VAD 统计 */
            voxa_get_metrics(handle, &metrics);
            jz_log_debug(JZ_APP_COM_MOD, "VOXA metrics: ERL=%.3f ERLE=%.3f delay=%d vad=%d\n",
                metrics.echo_return_loss,
                metrics.echo_return_loss_enhancement,
                metrics.delay_ms,
                metrics.vad_result);

            jz_time_stats_end(audio_cap_time_stats.echo_cancellation);
        }

distribute_frame:
        frame_header.channels = audio_config->params.channels;
        frame_header.bit_depth = pcm_data_sample_size(audio_config->params.pcm_data_fmt) * 8;
        frame_header.sample_rate = pcm_data_sample_rate(audio_config->params.pcm_sample_rate);
        frame_header.data_len = pack_size * 3;
        frame_header.multi_data = 1;
        frame_header.timestamp = systick_get_time_us();
        jz_audio_distribute(&frame_header, audio_buffer, frame_header.data_len);
    }

out:
    audio_lite_processor_deinit(&lite_processor);

    if (handle) {
        voxa_destroy(handle);
        handle = NULL;
    }

    if (controller_inited)
        audio_cap_controller_deinit();

    audio_cap_delay_fifo_deinit(&cap_delay_fifo);

    if (audio_buffer) {
        free(audio_buffer);
        audio_buffer = NULL;
    }

    jz_audio_run_status_set(&audio_cap_status, AUDIO_RUN_INIT, "audio_cap");
}

int audio_cap_status_get(void)
{
    return audio_cap_status == AUDIO_RUN_RUNNING;
}

int jz_audio_cap_set_3a_enabled(int enabled)
{
    if (audio_cap_status != AUDIO_RUN_INIT)
        return -1;

    audio_cap_3a_enabled = enabled != 0;
    jz_log_dump(JZ_APP_COM_MOD, "audio capture configure 3a:%d\n",
                audio_cap_3a_enabled);
    return 0;
}

int jz_audio_cap_start(void)
{
    thread_ptr_t worker;

    if (audio_cap_status != AUDIO_RUN_INIT) {
        jz_log_dump(JZ_APP_COM_MOD, "audio_cap is running!!!!\n");
        return 1;
    }
    jz_audio_cap_time_stats_init();
    worker = thread_create("audio_cap_thread", 64 * 1024,
                           audio_cap_thread, jz_get_audio_params());
    if (worker == NULL) {
        jz_time_stats_unregister_module(audio_cap_time_stats.module);
        jz_log_error(JZ_APP_COM_MOD, "audio_cap thread create failed\n");
        return -1;
    }

    return 0;
}

void jz_audio_cap_set_stop_status(void)
{
    jz_audio_run_status_set(&audio_cap_status, AUDIO_RUN_STOP, "audio_cap");
}

int jz_audio_cap_stop(void)
{
    jz_audio_run_status_wait(&audio_cap_status, AUDIO_RUN_INIT,  AUDIO_EXIT_TIMEOUT_MS, "audio_cap");
    jz_time_stats_unregister_module(audio_cap_time_stats.module);

    return 0;
}
