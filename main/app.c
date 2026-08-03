#include "app.h"
#include "mybot_config.h"
#include "audio/audio_device.h"
#include "protocols/rtc_session.h"
#include "protocols/device_state.h"
#include "ringbuf.h"

#include "api/aosl.h"
#include "api/aosl_mpq.h"
#include "api/aosl_mpq_timer.h"
#include "api/aosl_log.h"

#include <hal/aosl_hal_thread.h>

#include <string.h>
#include <stdlib.h>

/* ----------------------------------------------------------
 * Constants
 * ---------------------------------------------------------- */
#define SAMPLE_RATE       16000   /* Hz */
#define CHANNELS          1
#define BITS_PER_SAMPLE   16
#define FRAMES_20MS       320     /* samples per 20 ms frame @ 16 kHz */
#define BYTES_20MS        640     /* 320 * 16-bit mono */
#define RINGBUF_SIZE      (BYTES_20MS * 100)
#define AUDIO_TICK_MS     10      /* MPQ timer cadence driving the audio loops */
#define MPQ_STACK_SIZE    16384   /* 16 KB stack for aosl_mpq_create threads */

/* ----------------------------------------------------------
 * Global app state
 * ---------------------------------------------------------- */
static struct {
    const app_config_t *config;
    volatile bool       running;

    /* Audio capture */
    void           *cap_ctx;
    aosl_mpq_t      cap_mpq;      /* capture worker thread (aosl_mpq_create) */
    aosl_timer_t    cap_timer;    /* drives the capture read loop */
    ringbuf_t       cap_ringbuf;

    /* Audio playback */
    void           *pb_ctx;
    aosl_mpq_t      pb_mpq;       /* playback worker thread (aosl_mpq_create) */
    aosl_timer_t    pb_timer;     /* drives the playback write loop */
    ringbuf_t       pb_ringbuf;

#if MYBOT_CLOUD_AEC
    /* AEC reference ringbuf: holds downlink PCM fed to the speaker */
    ringbuf_t       ref_ringbuf;
#endif

    /* RTC session state */
    volatile bool   rtc_connected;
    char            rtc_app_id[64];
    char            rtc_channel[128];
    char            rtc_token[512];
    char            rtc_uid[64];

    /* MPQ handles */
    aosl_mpq_t      mpq;
    aosl_timer_t    send_timer;
} s_app;

/* ----------------------------------------------------------
 * Capture — runs on the capture MPQ thread (cap_mpq).
 * A periodic timer reads one 20 ms mic frame and feeds cap_ringbuf.
 * ---------------------------------------------------------- */
static void capture_timer(aosl_timer_t id, const aosl_ts_t *now,
                          uintptr_t argc, uintptr_t argv[])
{
    (void)id; (void)now; (void)argc; (void)argv;
    if (!s_app.running) return;

    const audio_capture_ops_t *ops = audio_device_get_capture();
    uint8_t pcm[BYTES_20MS];

    int frames = ops->read(s_app.cap_ctx, pcm, FRAMES_20MS);
    if (frames <= 0) return;

    /* Discard until RTC join succeeds (avoid filling ringbuf with stale data) */
    if (!s_app.rtc_connected) return;

    if (ringbuf_write(s_app.cap_ringbuf, (char *)pcm, BYTES_20MS) < 0) {
        static int dc = 0;
        if (++dc % 100 == 0) AOSL_LOG_WRN("cap ringbuf full, dropped %d", dc);
    }
}

static int cap_mpq_init(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("capture MPQ started");

    s_app.cap_timer = aosl_mpq_set_timer(AUDIO_TICK_MS, capture_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.cap_timer))
        AOSL_LOG_ERR("failed to create capture timer");

    return 0;
}

static void cap_mpq_fini(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("capture MPQ stopping");

    if (!aosl_mpq_timer_invalid(s_app.cap_timer)) {
        aosl_mpq_kill_timer(s_app.cap_timer);
        s_app.cap_timer = AOSL_MPQ_TIMER_INVALID;
    }
}

/* ----------------------------------------------------------
 * Playback — runs on the playback MPQ thread (pb_mpq).
 * A periodic timer pulls one 20 ms frame from pb_ringbuf and
 * writes it to the speaker.
 * ---------------------------------------------------------- */
static void playback_timer(aosl_timer_t id, const aosl_ts_t *now,
                           uintptr_t argc, uintptr_t argv[])
{
    (void)id; (void)now; (void)argc; (void)argv;
    if (!s_app.running) return;

    const audio_playback_ops_t *ops = audio_device_get_playback();
    uint8_t pcm[BYTES_20MS];

    if (ringbuf_get_data_size(s_app.pb_ringbuf) < BYTES_20MS) return;
    if (ringbuf_read((char *)pcm, BYTES_20MS, s_app.pb_ringbuf) != BYTES_20MS) return;
#if MYBOT_CLOUD_AEC
    /* Feed a copy to the AEC reference ringbuf before sending to speaker */
    ringbuf_write(s_app.ref_ringbuf, (char *)pcm, BYTES_20MS);
#endif
    ops->write(s_app.pb_ctx, pcm, FRAMES_20MS);
}

static int pb_mpq_init(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("playback MPQ started");

    s_app.pb_timer = aosl_mpq_set_timer(AUDIO_TICK_MS, playback_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.pb_timer))
        AOSL_LOG_ERR("failed to create playback timer");

    return 0;
}

static void pb_mpq_fini(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("playback MPQ stopping");

    if (!aosl_mpq_timer_invalid(s_app.pb_timer)) {
        aosl_mpq_kill_timer(s_app.pb_timer);
        s_app.pb_timer = AOSL_MPQ_TIMER_INVALID;
    }
}

/* ----------------------------------------------------------
 * RTC audio callback — SDK thread → playback ringbuf
 * ---------------------------------------------------------- */
static void on_remote_audio(uint32_t uid, const void *data, size_t len)
{
    (void)uid;
    if (!s_app.running) return;

    if (ringbuf_write(s_app.pb_ringbuf, (const char *)data, (int)len) < 0) {
        static int dc = 0;
        if (++dc % 100 == 0) AOSL_LOG_WRN("pb ringbuf full, dropped %d", dc);
    }
}

/* ----------------------------------------------------------
 * RTC state callback
 * ---------------------------------------------------------- */
static void on_rtc_state_changed(rtc_state_t state)
{
    s_app.rtc_connected = (state == RTC_STATE_CONNECTED);
    AOSL_LOG_INF("rtc -> %s", state == RTC_STATE_CONNECTED ? "connected" : "disconnected");
}

/* ----------------------------------------------------------
 * MPQ timer (20ms) — send captured PCM to RTC
 * ---------------------------------------------------------- */
static void send_audio_timer(aosl_timer_t id, const aosl_ts_t *now,
                             uintptr_t argc, uintptr_t argv[])
{
    (void)id; (void)now; (void)argc; (void)argv;

    if (!s_app.rtc_connected) return;

    uint8_t pcm[BYTES_20MS];
    if (ringbuf_get_data_size(s_app.cap_ringbuf) < BYTES_20MS) return;

    if (ringbuf_read((char *)pcm, BYTES_20MS, s_app.cap_ringbuf) == BYTES_20MS) {
#if MYBOT_CLOUD_AEC
        /* Interleave mic PCM with AEC reference (downlink PCM):
         * output = [mic[0], ref[0], mic[1], ref[1], ...] */
        int16_t *mic = (int16_t *)pcm;
        size_t   samples = BYTES_20MS / sizeof(int16_t);  /* 320 */
        int16_t  interleaved[BYTES_20MS * 2 / sizeof(int16_t)];  /* 640 samples */

        /* Use silence if insufficient ref data available */
        int16_t ref[320] = {0};
        if (ringbuf_get_data_size(s_app.ref_ringbuf) >= BYTES_20MS) {
            ringbuf_read((char *)ref, BYTES_20MS, s_app.ref_ringbuf);
        }

        for (size_t i = 0; i < samples; i++) {
            interleaved[i * 2]     = mic[i];
            interleaved[i * 2 + 1] = ref[i];
        }

        rtc_session_send_audio(interleaved, sizeof(interleaved));
#else
        rtc_session_send_audio(pcm, BYTES_20MS);
#endif
    }
}

/* ----------------------------------------------------------
 * Device state machine callbacks
 * ---------------------------------------------------------- */

static void dev_on_pair_code(const char *code)
{
    AOSL_LOG_INF("==== PAIR CODE ====");
    AOSL_LOG_INF("*** PAIR CODE: %s ***", code);
    AOSL_LOG_INF("*** Enter this code in the web UI to claim the device ***");
}

static void dev_on_conversation_start(const conversation_params_t *params)
{
    AOSL_LOG_INF("==== CONVERSATION START ====");
    AOSL_LOG_INF("  conversation_id: %s", params->conversation_id);
    AOSL_LOG_INF("  rtc channel    : %s", params->rtc_channel);
    AOSL_LOG_INF("  rtc uid        : %s", params->rtc_uid);
    AOSL_LOG_INF("  rtc app_id     : %s", params->rtc_app_id);
    AOSL_LOG_INF("  rtc token      : %s...", params->rtc_token);

    /* Save RTC params and join channel */
    strncpy(s_app.rtc_app_id,  params->rtc_app_id,   sizeof(s_app.rtc_app_id) - 1);
    strncpy(s_app.rtc_channel, params->rtc_channel,  sizeof(s_app.rtc_channel) - 1);
    strncpy(s_app.rtc_token,   params->rtc_token,    sizeof(s_app.rtc_token) - 1);
    strncpy(s_app.rtc_uid,     params->rtc_uid,      sizeof(s_app.rtc_uid) - 1);

    /* Init RTC session */
    rtc_session_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_remote_audio  = on_remote_audio;
    cbs.on_state_changed = on_rtc_state_changed;

    int ret = rtc_session_init(params->rtc_app_id, &cbs);
    if (ret < 0) {
        AOSL_LOG_ERR("rtc_session_init failed");
        return;
    }
    AOSL_LOG_INF("rtc_session_init ok");

    /* Join channel with server-assigned string UID */
    AOSL_LOG_INF("joining RTC as user_account=%s", params->rtc_uid);
    ret = rtc_session_join(params->rtc_channel, params->rtc_token, params->rtc_uid);
    if (ret < 0) {
        AOSL_LOG_ERR("rtc_session_join failed");
        return;
    }
    AOSL_LOG_INF("rtc_session_join requested, waiting for on_join_channel_success...");
}

static void dev_on_conversation_stop(void)
{
    AOSL_LOG_INF("==== CONVERSATION STOP ====");
    AOSL_LOG_INF("  channel: %s, uid: %s", s_app.rtc_channel, s_app.rtc_uid);

    int ret = rtc_session_leave();
    if (ret < 0) {
        AOSL_LOG_ERR("rtc_session_leave failed");
    } else {
        AOSL_LOG_INF("rtc_session_leave ok");
    }

    s_app.rtc_connected = false;
    AOSL_LOG_INF("==== CONVERSATION ENDED ====");
}

static void dev_on_state_changed(device_state_t state)
{
    (void)state;
}

/* ----------------------------------------------------------
 * MPQ init — runs inside MPQ thread at startup
 * ---------------------------------------------------------- */
static int mpq_init(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("MPQ loop started");

    s_app.send_timer = aosl_mpq_set_timer(20, send_audio_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.send_timer))
        AOSL_LOG_ERR("failed to create send timer");

    return 0;
}

/* ----------------------------------------------------------
 * MPQ fini — runs inside MPQ thread at shutdown
 * ---------------------------------------------------------- */
static void mpq_fini(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("MPQ loop stopping");

    if (!aosl_mpq_timer_invalid(s_app.send_timer)) {
        aosl_mpq_kill_timer(s_app.send_timer);
        s_app.send_timer = AOSL_MPQ_TIMER_INVALID;
    }

    rtc_session_leave();
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int app_start(const app_config_t *cfg)
{
    if (!cfg) return -1;

    memset(&s_app, 0, sizeof(s_app));
    s_app.config     = cfg;
    s_app.running    = true;
    s_app.mpq        = AOSL_MPQ_INVALID;
    s_app.send_timer = AOSL_MPQ_TIMER_INVALID;
    s_app.cap_mpq    = AOSL_MPQ_INVALID;
    s_app.cap_timer  = AOSL_MPQ_TIMER_INVALID;
    s_app.pb_mpq     = AOSL_MPQ_INVALID;
    s_app.pb_timer   = AOSL_MPQ_TIMER_INVALID;

    /* ---- 1. Initialize AOSL ---- */
    aosl_ctor();

    /* ---- 2. Initialize audio devices via the registered platform ops ----
     * The platform backend (e.g. ALSA on Linux) must have registered itself
     * through audio_device_register_*() before app_start() is called. */
    const audio_capture_ops_t  *cap_ops = audio_device_get_capture();
    const audio_playback_ops_t *pb_ops  = audio_device_get_playback();
    if (!cap_ops || !pb_ops) {
        AOSL_LOG_ERR("no audio platform registered");
        return -1;
    }

    if (cap_ops->init(&s_app.cap_ctx, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE) < 0) {
        AOSL_LOG_ERR("capture init failed");
        return -1;
    }
    if (pb_ops->init(&s_app.pb_ctx, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE) < 0) {
        AOSL_LOG_ERR("playback init failed");
        return -1;
    }

    /* ---- 3. Create ring buffers ---- */
    s_app.cap_ringbuf = ringbuf_create(RINGBUF_SIZE);
    s_app.pb_ringbuf  = ringbuf_create(RINGBUF_SIZE);
    if (!s_app.cap_ringbuf || !s_app.pb_ringbuf) {
        AOSL_LOG_ERR("ringbuf creation failed");
        return -1;
    }
#if MYBOT_CLOUD_AEC
    s_app.ref_ringbuf = ringbuf_create(RINGBUF_SIZE);
    if (!s_app.ref_ringbuf) {
        AOSL_LOG_ERR("ref ringbuf creation failed");
        return -1;
    }
    AOSL_LOG_INF("cloud AEC enabled, ref ringbuf created");
#endif

    /* ---- 4. Start audio devices ---- */
    cap_ops->start(s_app.cap_ctx);
    pb_ops->start(s_app.pb_ctx);

    /* ---- 5. Create the capture/playback worker threads ----
     * Each worker is an MPQ created with aosl_mpq_create(), which spawns the
     * thread and gives us join semantics through aosl_mpq_destroy_wait() —
     * the thread HAL (aosl_hal_thread_join) is not available on every
     * platform. The per-MPQ timer (cap_timer / pb_timer) drives the I/O. */
    s_app.cap_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 1000, "cap_mpq",
                                    cap_mpq_init, cap_mpq_fini, NULL);
    if (aosl_mpq_invalid(s_app.cap_mpq)) {
        AOSL_LOG_ERR("cap_mpq create failed");
        return -1;
    }

    s_app.pb_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 1000, "pb_mpq",
                                   pb_mpq_init, pb_mpq_fini, NULL);
    if (aosl_mpq_invalid(s_app.pb_mpq)) {
        AOSL_LOG_ERR("pb_mpq create failed");
        return -1;
    }

    /* ---- 6. Initialize the device state machine ---- */
    device_state_callbacks_t dev_cbs;
    memset(&dev_cbs, 0, sizeof(dev_cbs));
    dev_cbs.on_pair_code           = dev_on_pair_code;
    dev_cbs.on_conversation_start  = dev_on_conversation_start;
    dev_cbs.on_conversation_stop   = dev_on_conversation_stop;
    dev_cbs.on_state_changed       = dev_on_state_changed;

    if (device_state_init(cfg->server_base, cfg->device_id,
                          cfg->firmware_ver, cfg->hw_model,
                          &dev_cbs) < 0) {
        AOSL_LOG_ERR("device state init failed");
        return -1;
    }

    /* ---- 7. Create the MPQ and run its loop in a dedicated thread ----
     * Use aosl_mpq_create() instead of aosl_main_start(): the latter
     * registers an atexit() hook that re-runs aosl_main_exit_wait() after
     * main() returns, which aborts once aosl_dtor() has finalized AOSL.
     * Creating the queue explicitly keeps teardown fully in our control. */
    AOSL_LOG_INF("starting MPQ loop...");
    s_app.mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 10000, "mybot_mpq", mpq_init, mpq_fini, NULL);
    if (aosl_mpq_invalid(s_app.mpq)) {
        AOSL_LOG_ERR("aosl_mpq_create failed");
        return -1;
    }

    AOSL_LOG_INF("app started");
    return 0;
}

void app_tick(void)
{
    device_state_tick();
}

bool app_is_running(void)
{
    return s_app.running;
}

void app_request_exit(void)
{
    s_app.running = false;
}

void app_start_conversation(void)
{
    device_state_request_start();
}

void app_stop_conversation(void)
{
    device_state_request_stop();
}

void app_pair(void)
{
    device_state_request_pair();
}

void app_stop(void)
{
    AOSL_LOG_INF("stopping app...");

    /* ---- 1. Signal workers to stop ----
     * Set BEFORE any AOSL/audio teardown so the MPQ timer callbacks return
     * early. The ALSA read/write paths are poll-with-timeout, so each worker
     * exits within a bounded time even when the device yields no data. */
    s_app.running = false;

    /* ---- 2. Stop the MPQ loop ----
     * Its fini callback kills the send timer and leaves the RTC channel.
     * Must happen before tearing down RTC/audio. */
    if (!aosl_mpq_invalid(s_app.mpq)) {
        aosl_mpq_destroy_wait(s_app.mpq);
        s_app.mpq = AOSL_MPQ_INVALID;
    }

    /* ---- 3. Stop the capture/playback MPQ threads ----
     * aosl_mpq_destroy_wait() destroys the queue and joins its thread in one
     * call, replacing the thread-HAL join that is not portable. These must be
     * torn down before rtc_session_fini(): the RTC SDK finalizes AOSL itself
     * in agora_rtc_fini(), after which no AOSL call may be made. */
    if (!aosl_mpq_invalid(s_app.cap_mpq)) {
        aosl_mpq_destroy_wait(s_app.cap_mpq);
        s_app.cap_mpq = AOSL_MPQ_INVALID;
    }
    if (!aosl_mpq_invalid(s_app.pb_mpq)) {
        aosl_mpq_destroy_wait(s_app.pb_mpq);
        s_app.pb_mpq = AOSL_MPQ_INVALID;
    }

    /* ---- 4. Stop the RTC session ----
     * Also stops the SDK threads that feed the playback ring buffer. NOTE:
     * agora_rtc_fini() finalizes AOSL internally, so only AOSL-independent
     * teardown (devices, ring buffers) may follow. */
    rtc_session_fini();

    /* ---- 5. Destroy devices (no AOSL dependency) ---- */
    const audio_capture_ops_t  *cap_ops = audio_device_get_capture();
    const audio_playback_ops_t *pb_ops  = audio_device_get_playback();
    if (cap_ops && s_app.cap_ctx) { cap_ops->destroy(s_app.cap_ctx); s_app.cap_ctx = NULL; }
    if (pb_ops  && s_app.pb_ctx)  { pb_ops->destroy(s_app.pb_ctx);   s_app.pb_ctx  = NULL; }

    /* ---- 6. Destroy ring buffers ----
     * aosl_hal_free() maps to the system allocator on all platforms, so this
     * stays safe even after AOSL has been finalized by the RTC SDK. */
    if (s_app.cap_ringbuf) { ringbuf_destroy(s_app.cap_ringbuf); s_app.cap_ringbuf = NULL; }
    if (s_app.pb_ringbuf)  { ringbuf_destroy(s_app.pb_ringbuf);  s_app.pb_ringbuf  = NULL; }
#if MYBOT_CLOUD_AEC
    if (s_app.ref_ringbuf) { ringbuf_destroy(s_app.ref_ringbuf); s_app.ref_ringbuf = NULL; }
#endif

    /* ---- 7. Finalize AOSL ----
     * No-op if the RTC SDK already finalized AOSL in rtc_session_fini();
     * kept so the app also works when no SDK is involved. */
    aosl_dtor();
    AOSL_LOG_INF("app stopped cleanly");
}
