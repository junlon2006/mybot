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
/* Device state machine poll interval. Must match the 100 ms/tick assumption
 * in mybot_device_state_tick() (poll_after_seconds * 10 ticks). */
#define STATE_TICK_MS     100
#define MPQ_STACK_SIZE    16384   /* 16 KB stack for aosl_mpq_create threads */

/* ----------------------------------------------------------
 * Global app state
 * ---------------------------------------------------------- */
static struct {
    const mybot_app_config_t *config;
    volatile bool            running;

    /* Audio capture */
    void           *cap_ctx;
    aosl_mpq_t      cap_mpq;      /* capture worker thread (aosl_mpq_create) */
    aosl_timer_t    cap_timer;    /* drives the capture read loop */
    mybot_ringbuf_t cap_ringbuf;

    /* Audio playback */
    void           *pb_ctx;
    aosl_mpq_t      pb_mpq;       /* playback worker thread (aosl_mpq_create) */
    aosl_timer_t    pb_timer;     /* drives the playback write loop */
    mybot_ringbuf_t pb_ringbuf;

#if MYBOT_CLOUD_AEC
    /* AEC reference ringbuf: holds downlink PCM fed to the speaker */
    mybot_ringbuf_t ref_ringbuf;
#endif

    /* RTC session state */
    volatile bool   rtc_connected;
    char            rtc_app_id[64];
    char            rtc_channel[128];
    char            rtc_token[512];
    char            rtc_uid[64];

    /* MPQ handles — all real-time audio timers share this one thread */
    aosl_mpq_t      mpq;
    aosl_timer_t    send_timer;    /* 20 ms — send captured PCM to RTC */

    /* Device state machine MPQ — dedicated thread because
     * mybot_device_state_tick() does blocking HTTP polling that must not delay
     * the real-time audio timers on mybot_mpq. */
    aosl_mpq_t      state_mpq;
    aosl_timer_t    state_timer;   /* 100 ms — drive the device state machine */
} s_app;

/* ----------------------------------------------------------
 * Capture — runs on the capture MPQ thread (cap_mpq).
 * A periodic timer reads one 20 ms mic frame and feeds cap_ringbuf.
 * ---------------------------------------------------------- */
static void capture_timer(aosl_timer_t id, const aosl_ts_t *now,
                          uintptr_t argc, uintptr_t argv[])
{
    (void)id; (void)now; (void)argc; (void)argv;
    if (!s_app.running) { return; }

    const mybot_audio_capture_ops_t *ops = mybot_audio_device_get_capture();
    uint8_t pcm[BYTES_20MS];

    int frames = ops->read(s_app.cap_ctx, pcm, FRAMES_20MS);
    if (frames <= 0) { return; }

    /* Discard until RTC join succeeds (avoid filling ringbuf with stale data) */
    if (!s_app.rtc_connected) { return; }

    if (mybot_ringbuf_write(s_app.cap_ringbuf, (char *)pcm, BYTES_20MS) < 0) {
        static int dc = 0;
        if (++dc % 100 == 0) { AOSL_LOG_WRN("cap ringbuf full, dropped %d", dc); }
    }
}

static int cap_mpq_init(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("capture MPQ started");

    s_app.cap_timer = aosl_mpq_set_timer(AUDIO_TICK_MS, capture_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.cap_timer)) {
        AOSL_LOG_ERR("failed to create capture timer");
    }

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
    if (!s_app.running) { return; }

    const mybot_audio_playback_ops_t *ops = mybot_audio_device_get_playback();
    uint8_t pcm[BYTES_20MS];

    if (mybot_ringbuf_get_data_size(s_app.pb_ringbuf) < BYTES_20MS) { return; }
    if (mybot_ringbuf_read((char *)pcm, BYTES_20MS, s_app.pb_ringbuf) != BYTES_20MS) { return; }
#if MYBOT_CLOUD_AEC
    /* Feed a copy to the AEC reference ringbuf before sending to speaker */
    mybot_ringbuf_write(s_app.ref_ringbuf, (char *)pcm, BYTES_20MS);
#endif
    ops->write(s_app.pb_ctx, pcm, FRAMES_20MS);
}

static int pb_mpq_init(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("playback MPQ started");

    s_app.pb_timer = aosl_mpq_set_timer(AUDIO_TICK_MS, playback_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.pb_timer)) {
        AOSL_LOG_ERR("failed to create playback timer");
    }

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
    if (!s_app.running) { return; }

    if (mybot_ringbuf_write(s_app.pb_ringbuf, (const char *)data, (int)len) < 0) {
        static int dc = 0;
        if (++dc % 100 == 0) { AOSL_LOG_WRN("pb ringbuf full, dropped %d", dc); }
    }
}

/* ----------------------------------------------------------
 * RTC state callback
 * ---------------------------------------------------------- */
static void on_rtc_state_changed(mybot_rtc_state_t state)
{
    s_app.rtc_connected = (state == MYBOT_RTC_STATE_CONNECTED);
    AOSL_LOG_INF("rtc -> %s", state == MYBOT_RTC_STATE_CONNECTED ? "connected" : "disconnected");

    /* Unexpected RTC drop (connection lost / error): end the conversation.
     * mybot_device_state_notify_conversation_ended() only acts while the
     * device state machine is IN_CONVERSATION, so a deliberate 'q' stop
     * (state already RUNTIME) is never double-ended. RECONNECTING is transient
     * and is not treated as a drop. The teardown is deferred to the state_mpq
     * thread (this callback runs on an SDK thread). */
    if (state == MYBOT_RTC_STATE_DISCONNECTED || state == MYBOT_RTC_STATE_ERROR) {
        mybot_device_state_notify_conversation_ended();
    }
}

/* ----------------------------------------------------------
 * MPQ timer (20ms) — send captured PCM to RTC
 * ---------------------------------------------------------- */
static void send_audio_timer(aosl_timer_t id, const aosl_ts_t *now,
                             uintptr_t argc, uintptr_t argv[])
{
    (void)id; (void)now; (void)argc; (void)argv;

    if (!s_app.rtc_connected) { return; }

    uint8_t pcm[BYTES_20MS];
    if (mybot_ringbuf_get_data_size(s_app.cap_ringbuf) < BYTES_20MS) { return; }

    if (mybot_ringbuf_read((char *)pcm, BYTES_20MS, s_app.cap_ringbuf) == BYTES_20MS) {
#if MYBOT_CLOUD_AEC
        /* Interleave mic PCM with AEC reference (downlink PCM):
         * output = [mic[0], ref[0], mic[1], ref[1], ...] */
        int16_t *mic = (int16_t *)pcm;
        size_t   samples = BYTES_20MS / sizeof(int16_t);  /* 320 */
        int16_t  interleaved[BYTES_20MS * 2 / sizeof(int16_t)];  /* 640 samples */

        /* Use silence if insufficient ref data available */
        int16_t ref[320] = {0};
        if (mybot_ringbuf_get_data_size(s_app.ref_ringbuf) >= BYTES_20MS) {
            mybot_ringbuf_read((char *)ref, BYTES_20MS, s_app.ref_ringbuf);
        }

        for (size_t i = 0; i < samples; i++) {
            interleaved[i * 2]     = mic[i];
            interleaved[i * 2 + 1] = ref[i];
        }

        mybot_rtc_session_send_audio(interleaved, sizeof(interleaved));
#else
        mybot_rtc_session_send_audio(pcm, BYTES_20MS);
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

static void dev_on_conversation_start(const mybot_conversation_params_t *params)
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
    mybot_rtc_session_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_remote_audio  = on_remote_audio;
    cbs.on_state_changed = on_rtc_state_changed;

    int ret = mybot_rtc_session_init(params->rtc_app_id, &cbs);
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_rtc_session_init failed");
        /* The state machine already moved to IN_CONVERSATION before this
         * callback; roll it back so the device does not stay stuck. */
        mybot_device_state_notify_conversation_ended();
        return;
    }
    AOSL_LOG_INF("mybot_rtc_session_init ok");

    /* Join channel with server-assigned string UID */
    AOSL_LOG_INF("joining RTC as user_account=%s", params->rtc_uid);
    ret = mybot_rtc_session_join(params->rtc_channel, params->rtc_token, params->rtc_uid);
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_rtc_session_join failed");
        mybot_device_state_notify_conversation_ended();
        return;
    }
    AOSL_LOG_INF("mybot_rtc_session_join requested, waiting for on_join_channel_success...");
}

static void dev_on_conversation_stop(void)
{
    AOSL_LOG_INF("==== CONVERSATION STOP ====");
    AOSL_LOG_INF("  channel: %s, uid: %s", s_app.rtc_channel, s_app.rtc_uid);

    /* Stop the sender/capturer first so send_audio_timer stops attempting
     * sends before the connection is torn down. mybot_rtc_session_leave() is
     * additionally serialized against sends by an internal lock. */
    s_app.rtc_connected = false;

    int ret = mybot_rtc_session_leave();
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_rtc_session_leave failed");
    } else {
        AOSL_LOG_INF("mybot_rtc_session_leave ok");
    }

    AOSL_LOG_INF("==== CONVERSATION ENDED ====");
}

static void dev_on_state_changed(mybot_device_state_t state)
{
    (void)state;
}

/* Device state machine tick — runs on a dedicated MPQ (state_mpq) because
 * mybot_device_state_tick() performs blocking HTTP polling. */
static void state_tick_timer(aosl_timer_t id, const aosl_ts_t *now,
                             uintptr_t argc, uintptr_t argv[])
{
    (void)id; (void)now; (void)argc; (void)argv;
    mybot_device_state_tick();
}

static int state_mpq_init(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("state MPQ started");

    s_app.state_timer = aosl_mpq_set_timer(STATE_TICK_MS, state_tick_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.state_timer)) {
        AOSL_LOG_ERR("failed to create state timer");
    }

    return 0;
}

static void state_mpq_fini(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("state MPQ stopping");

    if (!aosl_mpq_timer_invalid(s_app.state_timer)) {
        aosl_mpq_kill_timer(s_app.state_timer);
        s_app.state_timer = AOSL_MPQ_TIMER_INVALID;
    }
}

/* ----------------------------------------------------------
 * MPQ init — runs inside MPQ thread at startup
 * ---------------------------------------------------------- */
static int mpq_init(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("MPQ loop started");

    s_app.send_timer = aosl_mpq_set_timer(20, send_audio_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.send_timer)) {
        AOSL_LOG_ERR("failed to create send timer");
    }

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

    mybot_rtc_session_leave();
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int mybot_app_start(const mybot_app_config_t *cfg)
{
    if (!cfg) { return -1; }

    memset(&s_app, 0, sizeof(s_app));
    s_app.config     = cfg;
    s_app.running    = true;
    s_app.mpq         = AOSL_MPQ_INVALID;
    s_app.send_timer  = AOSL_MPQ_TIMER_INVALID;
    s_app.cap_mpq     = AOSL_MPQ_INVALID;
    s_app.cap_timer   = AOSL_MPQ_TIMER_INVALID;
    s_app.pb_mpq      = AOSL_MPQ_INVALID;
    s_app.pb_timer    = AOSL_MPQ_TIMER_INVALID;
    s_app.state_mpq   = AOSL_MPQ_INVALID;
    s_app.state_timer = AOSL_MPQ_TIMER_INVALID;

    /* ---- 1. Initialize AOSL ---- */
    aosl_ctor();

    /* ---- 2. Initialize audio devices via the registered platform ops ----
     * The platform backend (e.g. ALSA on Linux) must have registered itself
     * through audio_device_register_*() before mybot_app_start() is called. */
    const mybot_audio_capture_ops_t  *cap_ops = mybot_audio_device_get_capture();
    const mybot_audio_playback_ops_t *pb_ops  = mybot_audio_device_get_playback();
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
    s_app.cap_ringbuf = mybot_ringbuf_create(RINGBUF_SIZE);
    s_app.pb_ringbuf  = mybot_ringbuf_create(RINGBUF_SIZE);
    if (!s_app.cap_ringbuf || !s_app.pb_ringbuf) {
        AOSL_LOG_ERR("ringbuf creation failed");
        return -1;
    }
#if MYBOT_CLOUD_AEC
    s_app.ref_ringbuf = mybot_ringbuf_create(RINGBUF_SIZE);
    if (!s_app.ref_ringbuf) {
        AOSL_LOG_ERR("ref ringbuf creation failed");
        return -1;
    }
    AOSL_LOG_INF("cloud AEC enabled, ref ringbuf created");
#endif

    /* ---- 4. Start audio devices ---- */
    cap_ops->start(s_app.cap_ctx);
    pb_ops->start(s_app.pb_ctx);

    /* ---- 5. Create the capture/playback worker MPQs ----
     * Each worker is an MPQ created with aosl_mpq_create(), which spawns the
     * thread and gives us join semantics through aosl_mpq_destroy_wait() —
     * the thread HAL (aosl_hal_thread_join) is not available on every
     * platform. The per-MPQ timer (cap_timer / pb_timer) drives the I/O on
     * its own thread, so blocking ALSA I/O never starves the audio sender. */
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
    mybot_device_state_callbacks_t dev_cbs;
    memset(&dev_cbs, 0, sizeof(dev_cbs));
    dev_cbs.on_pair_code           = dev_on_pair_code;
    dev_cbs.on_conversation_start  = dev_on_conversation_start;
    dev_cbs.on_conversation_stop   = dev_on_conversation_stop;
    dev_cbs.on_state_changed       = dev_on_state_changed;

    if (mybot_device_state_init(cfg->server_base, cfg->device_id,
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

    /* ---- 8. Create the device-state MPQ ----
     * Dedicated thread: mybot_device_state_tick() does blocking HTTP polling that
     * must not delay the real-time audio timers. */
    s_app.state_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 1000, "state_mpq",
                                      state_mpq_init, state_mpq_fini, NULL);
    if (aosl_mpq_invalid(s_app.state_mpq)) {
        AOSL_LOG_ERR("state_mpq create failed");
        return -1;
    }

    AOSL_LOG_INF("app started");
    return 0;
}

bool mybot_app_is_running(void)
{
    return s_app.running;
}

void mybot_app_request_exit(void)
{
    s_app.running = false;
}

void mybot_app_start_conversation(void)
{
    mybot_device_state_request_start();
}

void mybot_app_stop_conversation(void)
{
    mybot_device_state_request_stop();
}

void mybot_app_pair(void)
{
    mybot_device_state_request_pair();
}

void mybot_app_stop(void)
{
    AOSL_LOG_INF("stopping app...");

    /* ---- 1. Signal workers to stop ----
     * Set BEFORE any AOSL/audio teardown so the MPQ timer callbacks return
     * early. The ALSA read/write paths are poll-with-timeout, so each worker
     * exits within a bounded time even when the device yields no data. */
    s_app.running = false;

    /* ---- 2. Stop the MPQ loop ----
     * Its fini callback kills the send timer and leaves the RTC channel.
     * Must happen before mybot_rtc_session_fini(): the RTC SDK finalizes AOSL
     * itself in agora_rtc_fini(), after which no AOSL call may be made. */
    if (!aosl_mpq_invalid(s_app.mpq)) {
        aosl_mpq_destroy_wait(s_app.mpq);
        s_app.mpq = AOSL_MPQ_INVALID;
    }

    /* ---- 3. Stop the capture/playback MPQ threads ----
     * aosl_mpq_destroy_wait() destroys the queue and joins its thread in one
     * call, replacing the thread-HAL join that is not portable. These must be
     * torn down before mybot_rtc_session_fini() (which finalizes AOSL). */
    if (!aosl_mpq_invalid(s_app.cap_mpq)) {
        aosl_mpq_destroy_wait(s_app.cap_mpq);
        s_app.cap_mpq = AOSL_MPQ_INVALID;
    }
    if (!aosl_mpq_invalid(s_app.pb_mpq)) {
        aosl_mpq_destroy_wait(s_app.pb_mpq);
        s_app.pb_mpq = AOSL_MPQ_INVALID;
    }

    /* ---- 4. Stop the device-state MPQ ---- */
    if (!aosl_mpq_invalid(s_app.state_mpq)) {
        aosl_mpq_destroy_wait(s_app.state_mpq);
        s_app.state_mpq = AOSL_MPQ_INVALID;
    }

    /* ---- 5. Stop the RTC session ----
     * Also stops the SDK threads that feed the playback ring buffer. NOTE:
     * agora_rtc_fini() finalizes AOSL internally, so only AOSL-independent
     * teardown (devices, ring buffers) may follow. */
    mybot_rtc_session_fini();

    /* ---- 6. Destroy devices (no AOSL dependency) ---- */
    const mybot_audio_capture_ops_t  *cap_ops = mybot_audio_device_get_capture();
    const mybot_audio_playback_ops_t *pb_ops  = mybot_audio_device_get_playback();
    if (cap_ops && s_app.cap_ctx) { cap_ops->destroy(s_app.cap_ctx); s_app.cap_ctx = NULL; }
    if (pb_ops  && s_app.pb_ctx)  { pb_ops->destroy(s_app.pb_ctx);   s_app.pb_ctx  = NULL; }

    /* ---- 7. Destroy ring buffers ----
     * aosl_hal_free() maps to the system allocator on all platforms, so this
     * stays safe even after AOSL has been finalized by the RTC SDK. */
    if (s_app.cap_ringbuf) { mybot_ringbuf_destroy(s_app.cap_ringbuf); s_app.cap_ringbuf = NULL; }
    if (s_app.pb_ringbuf)  { mybot_ringbuf_destroy(s_app.pb_ringbuf);  s_app.pb_ringbuf  = NULL; }
#if MYBOT_CLOUD_AEC
    if (s_app.ref_ringbuf) { mybot_ringbuf_destroy(s_app.ref_ringbuf); s_app.ref_ringbuf = NULL; }
#endif

    /* ---- 8. Finalize AOSL ----
     * No-op if the RTC SDK already finalized AOSL in mybot_rtc_session_fini();
     * kept so the app also works when no SDK is involved. */
    aosl_dtor();
    AOSL_LOG_INF("app stopped cleanly");
}
