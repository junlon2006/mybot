#include "mybot_app.h"
#include "mybot_build_config.h"
#include "audio/mybot_audio_device.h"
#include "rtc/mybot_rtc_session.h"
#include "device/mybot_device_lifecycle.h"
#include "mybot_ringbuf.h"
#include "storage/mybot_kv_store.h"
#include "key_service/mybot_key_service.h"

#include "api/aosl.h"
#include "api/aosl_atomic.h"
#include "api/aosl_mpq.h"
#include "api/aosl_mpq_timer.h"
#include "api/aosl_log.h"

#include <hal/aosl_hal_thread.h>

#include <string.h>
#include <stdlib.h>

/* ----------------------------------------------------------
 * Constants
 * ---------------------------------------------------------- */
#define SAMPLE_RATE 16000 /* Hz */
#define CHANNELS 1
#define BITS_PER_SAMPLE 16
#define FRAMES_20MS 320 /* samples per 20 ms frame @ 16 kHz */
#define BYTES_20MS 640  /* 320 * 16-bit mono */
#define RINGBUF_SIZE (BYTES_20MS * 100)
#define AUDIO_TICK_MS 10 /* MPQ timer cadence driving the audio loops */
/* Device state machine poll interval. Must match the 100 ms/tick assumption
 * in mybot_device_lifecycle_tick() (poll_after_seconds * 10 ticks). */
#define STATE_TICK_MS 100
#define MPQ_STACK_SIZE 16384 /* 16 KB stack for aosl_mpq_create threads */

/* ----------------------------------------------------------
 * Global app state
 * ---------------------------------------------------------- */
static struct {
    aosl_atomic_t running;
    bool aosl_active;
    bool kv_store_active;
    bool key_service_active;

    /* Audio capture */
    void *cap_ctx;
    bool cap_started;
    aosl_mpq_t cap_mpq;     /* capture worker thread (aosl_mpq_create) */
    aosl_timer_t cap_timer; /* drives the capture read loop */
    mybot_ringbuf_t cap_ringbuf;

    /* Audio playback */
    void *pb_ctx;
    bool pb_started;
    aosl_mpq_t pb_mpq;     /* playback worker thread (aosl_mpq_create) */
    aosl_timer_t pb_timer; /* drives the playback write loop */
    mybot_ringbuf_t pb_ringbuf;
    uint8_t pb_pending[BYTES_20MS];
    int pb_pending_offset; /* frames already written */
    int pb_pending_frames; /* frames still to write */

#if MYBOT_CLOUD_AEC
    /* AEC reference ringbuf: holds downlink PCM fed to the speaker */
    mybot_ringbuf_t ref_ringbuf;
#endif

    /* RTC session state */
    aosl_atomic_t rtc_connected;
    char rtc_app_id[64];
    char rtc_channel[128];
    char rtc_token[512];
    char rtc_uid[64];

    /* MPQ handles — all real-time audio timers share this one thread */
    aosl_mpq_t mpq;
    aosl_timer_t send_timer; /* 20 ms — send captured PCM to RTC */

    /* Device state machine MPQ — dedicated thread because
     * mybot_device_lifecycle_tick() does blocking HTTP polling that must not delay
     * the real-time audio timers on mybot_mpq. */
    aosl_mpq_t state_mpq;
    aosl_timer_t state_timer; /* 100 ms — drive the device state machine */
} s_app;

/* ----------------------------------------------------------
 * Capture — runs on the capture MPQ thread (cap_mpq).
 * A periodic timer reads one 20 ms mic frame and feeds cap_ringbuf.
 * ---------------------------------------------------------- */
static void capture_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc, uintptr_t argv[]) {
    (void)id;
    (void)now;
    (void)argc;
    (void)argv;
    if (!aosl_atomic_read(&s_app.running)) {
        return;
    }

    const mybot_audio_capture_ops_t *ops = mybot_audio_device_get_capture();
    uint8_t pcm[BYTES_20MS];

    int frames = ops->read(s_app.cap_ctx, pcm, FRAMES_20MS);
    if (frames <= 0) {
        return;
    }
    if (frames > FRAMES_20MS) {
        AOSL_LOG_ERR("capture backend returned invalid frame count: %d > %d", frames, FRAMES_20MS);
        return;
    }

    /* Discard until RTC join succeeds (avoid filling ringbuf with stale data) */
    if (!aosl_atomic_read(&s_app.rtc_connected)) {
        return;
    }

    const int frame_bytes = CHANNELS * BITS_PER_SAMPLE / 8;
    const int bytes_read = frames * frame_bytes;
    if (mybot_ringbuf_write(s_app.cap_ringbuf, (char *)pcm, bytes_read) < 0) {
        static int dc = 0;
        if (++dc % 100 == 0) {
            AOSL_LOG_WRN("cap ringbuf full, dropped %d", dc);
        }
    }
}

static int cap_mpq_init(void *arg) {
    (void)arg;
    AOSL_LOG_INF("capture MPQ started");

    s_app.cap_timer = aosl_mpq_set_timer(AUDIO_TICK_MS, capture_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.cap_timer)) {
        AOSL_LOG_ERR("failed to create capture timer");
        return -1;
    }

    return 0;
}

static void cap_mpq_fini(void *arg) {
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
static void playback_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc,
                           uintptr_t argv[]) {
    (void)id;
    (void)now;
    (void)argc;
    (void)argv;
    if (!aosl_atomic_read(&s_app.running)) {
        return;
    }

    const mybot_audio_playback_ops_t *ops = mybot_audio_device_get_playback();

    if (s_app.pb_pending_frames == 0) {
        if (mybot_ringbuf_get_data_size(s_app.pb_ringbuf) < BYTES_20MS) {
            return;
        }
        if (mybot_ringbuf_read((char *)s_app.pb_pending, BYTES_20MS, s_app.pb_ringbuf) !=
            BYTES_20MS) {
            return;
        }
        s_app.pb_pending_offset = 0;
        s_app.pb_pending_frames = FRAMES_20MS;
#if MYBOT_CLOUD_AEC
        /* Preserve the existing AEC reference timing: publish once when the
         * playback frame is first dequeued. */
        mybot_ringbuf_write(s_app.ref_ringbuf, (char *)s_app.pb_pending, BYTES_20MS);
#endif
    }

    const int frame_bytes = CHANNELS * BITS_PER_SAMPLE / 8;
    int written = ops->write(s_app.pb_ctx, s_app.pb_pending + s_app.pb_pending_offset * frame_bytes,
                             s_app.pb_pending_frames);
    if (written < 0) {
        AOSL_LOG_ERR("playback write failed, dropping %d pending frames", s_app.pb_pending_frames);
        s_app.pb_pending_offset = 0;
        s_app.pb_pending_frames = 0;
        return;
    }
    if (written == 0) {
        return;
    }
    if (written > s_app.pb_pending_frames) {
        AOSL_LOG_ERR("playback backend returned invalid frame count: %d > %d", written,
                     s_app.pb_pending_frames);
        s_app.pb_pending_offset = 0;
        s_app.pb_pending_frames = 0;
        return;
    }

    s_app.pb_pending_offset += written;
    s_app.pb_pending_frames -= written;
    if (s_app.pb_pending_frames == 0) {
        s_app.pb_pending_offset = 0;
    }
}

static int pb_mpq_init(void *arg) {
    (void)arg;
    AOSL_LOG_INF("playback MPQ started");

    s_app.pb_timer = aosl_mpq_set_timer(AUDIO_TICK_MS, playback_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.pb_timer)) {
        AOSL_LOG_ERR("failed to create playback timer");
        return -1;
    }

    return 0;
}

static void pb_mpq_fini(void *arg) {
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
static void on_remote_audio(uint32_t uid, const void *data, size_t len) {
    (void)uid;
    if (!aosl_atomic_read(&s_app.running)) {
        return;
    }

    if (mybot_ringbuf_write(s_app.pb_ringbuf, (const char *)data, (int)len) < 0) {
        AOSL_LOG_WRN("pb ringbuf full, dropped");
    }
}

/* ----------------------------------------------------------
 * RTC state callback
 * ---------------------------------------------------------- */
static void on_rtc_state_changed(mybot_rtc_state_t state) {
    aosl_atomic_set(&s_app.rtc_connected, state == MYBOT_RTC_STATE_CONNECTED);
    AOSL_LOG_INF("rtc -> %s", state == MYBOT_RTC_STATE_CONNECTED ? "connected" : "disconnected");

    /* Unexpected RTC drop (connection lost / error): end the conversation.
     * mybot_device_lifecycle_notify_conversation_ended() only acts while the
     * device state machine is IN_CONVERSATION, so a deliberate 'q' stop
     * (state already RUNTIME) is never double-ended. RECONNECTING is transient
     * and is not treated as a drop. The teardown is deferred to the state_mpq
     * thread (this callback runs on an SDK thread). */
    if (state == MYBOT_RTC_STATE_DISCONNECTED || state == MYBOT_RTC_STATE_ERROR) {
        mybot_device_lifecycle_notify_conversation_ended();
    }
}

/* ----------------------------------------------------------
 * MPQ timer (20ms) — send captured PCM to RTC
 * ---------------------------------------------------------- */
static void send_audio_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc,
                             uintptr_t argv[]) {
    (void)id;
    (void)now;
    (void)argc;
    (void)argv;

    if (!aosl_atomic_read(&s_app.rtc_connected)) {
        return;
    }

    uint8_t pcm[BYTES_20MS];
    if (mybot_ringbuf_get_data_size(s_app.cap_ringbuf) < BYTES_20MS) {
        return;
    }

    if (mybot_ringbuf_read((char *)pcm, BYTES_20MS, s_app.cap_ringbuf) == BYTES_20MS) {
#if MYBOT_CLOUD_AEC
        /* Interleave mic PCM with AEC reference (downlink PCM):
         * output = [mic[0], ref[0], mic[1], ref[1], ...] */
        int16_t *mic = (int16_t *)pcm;
        size_t samples = BYTES_20MS / sizeof(int16_t);         /* 320 */
        int16_t interleaved[BYTES_20MS * 2 / sizeof(int16_t)]; /* 640 samples */

        /* Use silence if insufficient ref data available */
        int16_t ref[320] = {0};
        if (mybot_ringbuf_get_data_size(s_app.ref_ringbuf) >= BYTES_20MS) {
            mybot_ringbuf_read((char *)ref, BYTES_20MS, s_app.ref_ringbuf);
        }

        for (size_t i = 0; i < samples; i++) {
            interleaved[i * 2] = mic[i];
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

static void dev_on_pair_code(const char *code) {
    AOSL_LOG_INF("==== PAIR CODE ====");
    AOSL_LOG_INF("*** PAIR CODE: %s ***", code);
    AOSL_LOG_INF("*** Enter this code in the web UI to claim the device ***");
}

static void dev_on_conversation_start(const mybot_conversation_params_t *params) {
    if (!aosl_atomic_read(&s_app.running)) {
        AOSL_LOG_INF("ignoring conversation start during shutdown");
        mybot_device_lifecycle_notify_conversation_ended();
        return;
    }

    AOSL_LOG_INF("==== CONVERSATION START ====");
    AOSL_LOG_INF("  conversation_id: %s", params->conversation_id);
    AOSL_LOG_INF("  rtc channel    : %s", params->rtc_channel);
    AOSL_LOG_INF("  rtc uid        : %s", params->rtc_uid);
    AOSL_LOG_INF("  rtc app_id     : %s", params->rtc_app_id);
    AOSL_LOG_INF("  rtc token      : %s...", params->rtc_token);

    /* Save RTC params and join channel */
    strncpy(s_app.rtc_app_id, params->rtc_app_id, sizeof(s_app.rtc_app_id) - 1);
    strncpy(s_app.rtc_channel, params->rtc_channel, sizeof(s_app.rtc_channel) - 1);
    strncpy(s_app.rtc_token, params->rtc_token, sizeof(s_app.rtc_token) - 1);
    strncpy(s_app.rtc_uid, params->rtc_uid, sizeof(s_app.rtc_uid) - 1);

    /* Init RTC session */
    mybot_rtc_session_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_remote_audio = on_remote_audio;
    cbs.on_state_changed = on_rtc_state_changed;

    int ret = mybot_rtc_session_init(params->rtc_app_id, &cbs);
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_rtc_session_init failed");
        /* The state machine already moved to IN_CONVERSATION before this
         * callback; roll it back so the device does not stay stuck. */
        mybot_device_lifecycle_notify_conversation_ended();
        return;
    }
    AOSL_LOG_INF("mybot_rtc_session_init ok");

    /* Join channel with server-assigned string UID */
    AOSL_LOG_INF("joining RTC as user_account=%s", params->rtc_uid);
    ret = mybot_rtc_session_join(params->rtc_channel, params->rtc_token, params->rtc_uid);
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_rtc_session_join failed");
        mybot_device_lifecycle_notify_conversation_ended();
        return;
    }
    AOSL_LOG_INF("mybot_rtc_session_join requested, waiting for on_join_channel_success...");
}

static void dev_on_conversation_stop(void) {
    AOSL_LOG_INF("==== CONVERSATION STOP ====");
    AOSL_LOG_INF("  channel: %s, uid: %s", s_app.rtc_channel, s_app.rtc_uid);

    /* Stop the sender/capturer first so send_audio_timer stops attempting
     * sends before the connection is torn down. mybot_rtc_session_leave() is
     * additionally serialized against sends by an internal lock. */
    aosl_atomic_set(&s_app.rtc_connected, false);

    int ret = mybot_rtc_session_leave();
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_rtc_session_leave failed");
    } else {
        AOSL_LOG_INF("mybot_rtc_session_leave ok");
    }

    AOSL_LOG_INF("==== CONVERSATION ENDED ====");
}

static void dev_on_state_changed(mybot_device_state_t state) {
    (void)state;
}

static void on_key_event(mybot_key_event_t event, void *user_data) {
    (void)user_data;
    switch (event) {
    case MYBOT_KEY_EVENT_CONVERSATION_START:
        AOSL_LOG_INF("[KEY] start conversation");
        mybot_app_start_conversation();
        break;
    case MYBOT_KEY_EVENT_CONVERSATION_STOP:
        AOSL_LOG_INF("[KEY] stop conversation");
        mybot_app_stop_conversation();
        break;
    case MYBOT_KEY_EVENT_PAIR:
        AOSL_LOG_INF("[KEY] re-pair");
        mybot_app_pair();
        break;
    case MYBOT_KEY_EVENT_EXIT:
        AOSL_LOG_INF("[KEY] exit");
        mybot_app_request_exit();
        break;
    }
}

/* Device state machine tick — runs on a dedicated MPQ (state_mpq) because
 * mybot_device_lifecycle_tick() performs blocking HTTP polling. */
static void state_tick_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc,
                             uintptr_t argv[]) {
    (void)id;
    (void)now;
    (void)argc;
    (void)argv;
    if (!aosl_atomic_read(&s_app.running)) {
        return;
    }
    mybot_device_lifecycle_tick();
}

static int state_mpq_init(void *arg) {
    (void)arg;
    AOSL_LOG_INF("state MPQ started");

    s_app.state_timer = aosl_mpq_set_timer(STATE_TICK_MS, state_tick_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.state_timer)) {
        AOSL_LOG_ERR("failed to create state timer");
        return -1;
    }

    return 0;
}

static void state_mpq_fini(void *arg) {
    (void)arg;
    AOSL_LOG_INF("state MPQ stopping");

    if (!aosl_mpq_timer_invalid(s_app.state_timer)) {
        aosl_mpq_kill_timer(s_app.state_timer);
        s_app.state_timer = AOSL_MPQ_TIMER_INVALID;
    }

    mybot_device_lifecycle_shutdown();
}

/* ----------------------------------------------------------
 * MPQ init — runs inside MPQ thread at startup
 * ---------------------------------------------------------- */
static int mpq_init(void *arg) {
    (void)arg;
    AOSL_LOG_INF("MPQ loop started");

    s_app.send_timer = aosl_mpq_set_timer(20, send_audio_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.send_timer)) {
        AOSL_LOG_ERR("failed to create send timer");
        return -1;
    }

    return 0;
}

/* ----------------------------------------------------------
 * MPQ fini — runs inside MPQ thread at shutdown
 * ---------------------------------------------------------- */
static void mpq_fini(void *arg) {
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

int mybot_app_start(const mybot_app_config_t *cfg) {
    if (!cfg) {
        return -1;
    }

    memset(&s_app, 0, sizeof(s_app));
    aosl_atomic_set(&s_app.running, true);
    s_app.mpq = AOSL_MPQ_INVALID;
    s_app.send_timer = AOSL_MPQ_TIMER_INVALID;
    s_app.cap_mpq = AOSL_MPQ_INVALID;
    s_app.cap_timer = AOSL_MPQ_TIMER_INVALID;
    s_app.pb_mpq = AOSL_MPQ_INVALID;
    s_app.pb_timer = AOSL_MPQ_TIMER_INVALID;
    s_app.state_mpq = AOSL_MPQ_INVALID;
    s_app.state_timer = AOSL_MPQ_TIMER_INVALID;

    /* ---- 1. Initialize AOSL ---- */
    aosl_ctor();
    s_app.aosl_active = true;

    if (mybot_kv_store_init() < 0) {
        AOSL_LOG_ERR("kv store init failed");
        goto fail;
    }
    s_app.kv_store_active = true;

    if (mybot_key_service_init(on_key_event, NULL) < 0) {
        AOSL_LOG_ERR("key service init failed");
        goto fail;
    }
    s_app.key_service_active = true;

    /* ---- 2. Initialize audio devices via the registered platform ops ----
     * The platform backend (e.g. ALSA on Linux) must have registered itself
     * through audio_device_register_*() before mybot_app_start() is called. */
    const mybot_audio_capture_ops_t *cap_ops = mybot_audio_device_get_capture();
    const mybot_audio_playback_ops_t *pb_ops = mybot_audio_device_get_playback();
    if (!cap_ops || !pb_ops) {
        AOSL_LOG_ERR("no audio platform registered");
        goto fail;
    }

    if (cap_ops->init(&s_app.cap_ctx, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE) < 0) {
        AOSL_LOG_ERR("capture init failed");
        goto fail;
    }
    if (pb_ops->init(&s_app.pb_ctx, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE) < 0) {
        AOSL_LOG_ERR("playback init failed");
        goto fail;
    }

    /* ---- 3. Create ring buffers ---- */
    s_app.cap_ringbuf = mybot_ringbuf_create(RINGBUF_SIZE);
    s_app.pb_ringbuf = mybot_ringbuf_create(RINGBUF_SIZE);
    if (!s_app.cap_ringbuf || !s_app.pb_ringbuf) {
        AOSL_LOG_ERR("ringbuf creation failed");
        goto fail;
    }
#if MYBOT_CLOUD_AEC
    s_app.ref_ringbuf = mybot_ringbuf_create(RINGBUF_SIZE);
    if (!s_app.ref_ringbuf) {
        AOSL_LOG_ERR("ref ringbuf creation failed");
        goto fail;
    }
    AOSL_LOG_INF("cloud AEC enabled, ref ringbuf created");
#endif

    /* ---- 4. Start audio devices ---- */
    if (!cap_ops->start || cap_ops->start(s_app.cap_ctx) < 0) {
        AOSL_LOG_ERR("capture start failed");
        goto fail;
    }
    s_app.cap_started = true;

    if (!pb_ops->start || pb_ops->start(s_app.pb_ctx) < 0) {
        AOSL_LOG_ERR("playback start failed");
        goto fail;
    }
    s_app.pb_started = true;

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
        goto fail;
    }

    s_app.pb_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 1000, "pb_mpq",
                                   pb_mpq_init, pb_mpq_fini, NULL);
    if (aosl_mpq_invalid(s_app.pb_mpq)) {
        AOSL_LOG_ERR("pb_mpq create failed");
        goto fail;
    }

    /* ---- 6. Initialize the device state machine ---- */
    mybot_device_lifecycle_callbacks_t dev_cbs;
    memset(&dev_cbs, 0, sizeof(dev_cbs));
    dev_cbs.on_pair_code = dev_on_pair_code;
    dev_cbs.on_conversation_start = dev_on_conversation_start;
    dev_cbs.on_conversation_stop = dev_on_conversation_stop;
    dev_cbs.on_state_changed = dev_on_state_changed;

    if (mybot_device_lifecycle_init(cfg->server_base, cfg->device_id, cfg->firmware_ver,
                                    cfg->hw_model, &dev_cbs) < 0) {
        AOSL_LOG_ERR("device state init failed");
        goto fail;
    }

    /* ---- 7. Create the MPQ and run its loop in a dedicated thread ----
     * Use aosl_mpq_create() instead of aosl_main_start(): the latter
     * registers an atexit() hook that re-runs aosl_main_exit_wait() after
     * main() returns, which aborts once aosl_dtor() has finalized AOSL.
     * Creating the queue explicitly keeps teardown fully in our control. */
    AOSL_LOG_INF("starting MPQ loop...");
    s_app.mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 10000, "mybot_mpq", mpq_init,
                                mpq_fini, NULL);
    if (aosl_mpq_invalid(s_app.mpq)) {
        AOSL_LOG_ERR("aosl_mpq_create failed");
        goto fail;
    }

    /* ---- 8. Create the device-state MPQ ----
     * Dedicated thread: mybot_device_lifecycle_tick() does blocking HTTP polling that
     * must not delay the real-time audio timers. */
    s_app.state_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 1000, "state_mpq",
                                      state_mpq_init, state_mpq_fini, NULL);
    if (aosl_mpq_invalid(s_app.state_mpq)) {
        AOSL_LOG_ERR("state_mpq create failed");
        goto fail;
    }

    AOSL_LOG_INF("app started");
    return 0;

fail:
    mybot_app_stop();
    return -1;
}

bool mybot_app_is_running(void) {
    return aosl_atomic_read(&s_app.running) != 0;
}

void mybot_app_poll(void) {
    if (s_app.key_service_active && mybot_key_service_poll() < 0) {
        AOSL_LOG_ERR("key service poll failed");
    }
}

void mybot_app_request_exit(void) {
    aosl_atomic_set(&s_app.running, false);
}

void mybot_app_start_conversation(void) {
    mybot_device_lifecycle_request_start();
}

void mybot_app_stop_conversation(void) {
    mybot_device_lifecycle_request_stop();
}

void mybot_app_pair(void) {
    mybot_device_lifecycle_request_pair();
}

void mybot_app_stop(void) {
    /* Keep stop idempotent without touching AOSL after a previous stop. */
    if (!s_app.aosl_active) {
        return;
    }

    AOSL_LOG_INF("stopping app...");

    /* ---- 1. Signal workers to stop ----
     * Set BEFORE any AOSL/audio teardown so the MPQ timer callbacks return
     * early. The ALSA read/write paths are poll-with-timeout, so each worker
     * exits within a bounded time even when the device yields no data. */
    aosl_atomic_set(&s_app.running, false);

    if (s_app.key_service_active) {
        mybot_key_service_deinit();
        s_app.key_service_active = false;
    }

    /* ---- 2. Stop device-state activity ----
     * Wait for any in-flight HTTP operation, prevent further state-machine
     * actions, and close an active server conversation before RTC/audio
     * resources are dismantled. */
    if (!aosl_mpq_invalid(s_app.state_mpq)) {
        aosl_mpq_destroy_wait(s_app.state_mpq);
        s_app.state_mpq = AOSL_MPQ_INVALID;
    }

    /* ---- 3. Stop the MPQ loop ----
     * Its fini callback kills the send timer and leaves the RTC channel.
     * Must happen before mybot_rtc_session_fini(): the RTC SDK finalizes AOSL
     * itself in agora_rtc_fini(), after which no AOSL call may be made. */
    if (!aosl_mpq_invalid(s_app.mpq)) {
        aosl_mpq_destroy_wait(s_app.mpq);
        s_app.mpq = AOSL_MPQ_INVALID;
    }

    /* ---- 4. Stop the capture/playback MPQ threads ----
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

    if (s_app.kv_store_active) {
        mybot_kv_store_deinit();
        s_app.kv_store_active = false;
    }

    /* ---- 5. Stop audio devices ----
     * Their worker MPQs have exited, so no read/write can race with stop. */
    const mybot_audio_capture_ops_t *cap_ops = mybot_audio_device_get_capture();
    const mybot_audio_playback_ops_t *pb_ops = mybot_audio_device_get_playback();
    if (s_app.pb_started) {
        if (pb_ops && pb_ops->stop && pb_ops->stop(s_app.pb_ctx) < 0) {
            AOSL_LOG_ERR("playback stop failed");
        }
        s_app.pb_started = false;
    }
    if (s_app.cap_started) {
        if (cap_ops && cap_ops->stop && cap_ops->stop(s_app.cap_ctx) < 0) {
            AOSL_LOG_ERR("capture stop failed");
        }
        s_app.cap_started = false;
    }

    /* ---- 6. Destroy devices while AOSL logging is still available ---- */
    if (cap_ops && s_app.cap_ctx) {
        cap_ops->destroy(s_app.cap_ctx);
        s_app.cap_ctx = NULL;
    }
    if (pb_ops && s_app.pb_ctx) {
        pb_ops->destroy(s_app.pb_ctx);
        s_app.pb_ctx = NULL;
    }

    /* ---- 7. Finalize RTC ----
     * The SDK waits for its callback queue before returning, so no callback can
     * access pb_ringbuf after this point. It also finalizes AOSL when active. */
    AOSL_LOG_INF("app stopped cleanly");
    s_app.aosl_active = false;
    bool rtc_finalized_aosl = mybot_rtc_session_fini();

    /* ---- 8. Destroy ring buffers ----
     * The AOSL HAL allocator is independent of the AOSL global lifecycle. */
    if (s_app.cap_ringbuf) {
        mybot_ringbuf_destroy(s_app.cap_ringbuf);
        s_app.cap_ringbuf = NULL;
    }
    if (s_app.pb_ringbuf) {
        mybot_ringbuf_destroy(s_app.pb_ringbuf);
        s_app.pb_ringbuf = NULL;
    }
#if MYBOT_CLOUD_AEC
    if (s_app.ref_ringbuf) {
        mybot_ringbuf_destroy(s_app.ref_ringbuf);
        s_app.ref_ringbuf = NULL;
    }
#endif

    /* ---- 9. Finalize AOSL when RTC never owned it ---- */
    if (!rtc_finalized_aosl) {
        aosl_dtor();
    }
}
