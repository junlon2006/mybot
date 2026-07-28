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
#include <hal/aosl_hal_time.h>
#include <hal/aosl_hal_socket.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

/* ----------------------------------------------------------
 * Constants
 * ---------------------------------------------------------- */
#define SAMPLE_RATE       16000
#define CHANNELS          1
#define BITS_PER_SAMPLE   16
#define FRAMES_20MS       320
#define BYTES_20MS        640
#define RINGBUF_SIZE      (BYTES_20MS * 100)

/* ----------------------------------------------------------
 * Platform registration forward declarations
 * ---------------------------------------------------------- */
void audio_platform_register_alsa_capture(void);
void audio_platform_register_alsa_playback(void);

/* ----------------------------------------------------------
 * Global app state
 * ---------------------------------------------------------- */
static struct {
    const app_config_t *config;
    volatile bool       running;

    /* Audio capture */
    void           *cap_ctx;
    aosl_thread_t   cap_thread;
    ringbuf_t       cap_ringbuf;

    /* Audio playback */
    void           *pb_ctx;
    aosl_thread_t   pb_thread;
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

    /* Timer handles */
    aosl_timer_t    send_timer;

    /* Audio device lifecycle lock */
    aosl_mutex_t    lock;
} s_app;

/* ----------------------------------------------------------
 * Signal handler
 * ---------------------------------------------------------- */
static void signal_handler(int sig)
{
    (void)sig;
    AOSL_LOG_INF("caught signal, stopping...");
    s_app.running = false;
}

/* ----------------------------------------------------------
 * Capture worker — reads mic PCM → capture ringbuf
 * ---------------------------------------------------------- */
static void *capture_worker(void *arg)
{
    (void)arg;
    const audio_capture_ops_t *ops = audio_device_get_capture();
    uint8_t pcm[BYTES_20MS];

    AOSL_LOG_INF("capture worker started");
    while (s_app.running) {
        int frames = ops->read(s_app.cap_ctx, pcm, FRAMES_20MS);
        if (frames <= 0) { aosl_hal_msleep(5); continue; }

        if (ringbuf_write(s_app.cap_ringbuf, (char *)pcm, BYTES_20MS) < 0) {
            static int dc = 0;
            if (++dc % 100 == 0) AOSL_LOG_WRN("cap ringbuf full, dropped %d", dc);
        }
    }
    AOSL_LOG_INF("capture worker stopped");
    return NULL;
}

/* ----------------------------------------------------------
 * Playback worker — reads playback ringbuf → speaker PCM
 * ---------------------------------------------------------- */
static void *playback_worker(void *arg)
{
    (void)arg;
    const audio_playback_ops_t *ops = audio_device_get_playback();
    uint8_t pcm[BYTES_20MS];

    AOSL_LOG_INF("playback worker started");
    while (s_app.running) {
        int avail = ringbuf_get_data_size(s_app.pb_ringbuf);
        if (avail < BYTES_20MS) { aosl_hal_msleep(5); continue; }

        if (ringbuf_read((char *)pcm, BYTES_20MS, s_app.pb_ringbuf) == BYTES_20MS) {
#if MYBOT_CLOUD_AEC
            /* Feed a copy to the AEC reference ringbuf before sending to speaker */
            ringbuf_write(s_app.ref_ringbuf, (char *)pcm, BYTES_20MS);
#endif
            ops->write(s_app.pb_ctx, pcm, FRAMES_20MS);
        }
    }
    AOSL_LOG_INF("playback worker stopped");
    return NULL;
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

static void dev_on_audio_start(void)
{
    AOSL_LOG_INF("audio start");
    const audio_capture_ops_t  *cap_ops = audio_device_get_capture();
    const audio_playback_ops_t *pb_ops  = audio_device_get_playback();
    if (!cap_ops || !pb_ops) { AOSL_LOG_ERR("audio device not registered"); return; }

    cap_ops->start(s_app.cap_ctx);
    pb_ops->start(s_app.pb_ctx);

    s_app.running = true;

    aosl_thread_param_t p_cap = { "cap_worker", AOSL_THRD_PRI_NORMAL, 0 };
    aosl_hal_thread_create(&s_app.cap_thread, &p_cap, capture_worker, NULL);

    aosl_thread_param_t p_pb = { "pb_worker", AOSL_THRD_PRI_NORMAL, 0 };
    aosl_hal_thread_create(&s_app.pb_thread, &p_pb, playback_worker, NULL);
}

static void dev_on_audio_stop(void)
{
    AOSL_LOG_INF("audio stop");
    const audio_capture_ops_t  *cap_ops = audio_device_get_capture();
    const audio_playback_ops_t *pb_ops  = audio_device_get_playback();

    /* Close PCM devices to unblock capture/playback threads */
    if (cap_ops && s_app.cap_ctx) { cap_ops->stop(s_app.cap_ctx); cap_ops->destroy(s_app.cap_ctx); s_app.cap_ctx = NULL; }
    if (pb_ops && s_app.pb_ctx)  { pb_ops->stop(s_app.pb_ctx);  pb_ops->destroy(s_app.pb_ctx);  s_app.pb_ctx  = NULL; }

    s_app.running = false;
    aosl_hal_msleep(50);

    if (s_app.cap_thread) { aosl_hal_thread_join(s_app.cap_thread, NULL); s_app.cap_thread = 0; }
    if (s_app.pb_thread)  { aosl_hal_thread_join(s_app.pb_thread, NULL);  s_app.pb_thread  = 0; }

    /* Clear all ring buffers */
    if (s_app.cap_ringbuf) ringbuf_clear(s_app.cap_ringbuf);
    if (s_app.pb_ringbuf)  ringbuf_clear(s_app.pb_ringbuf);
#if MYBOT_CLOUD_AEC
    if (s_app.ref_ringbuf) ringbuf_clear(s_app.ref_ringbuf);
#endif
    AOSL_LOG_INF("audio stop complete, ringbufs cleared");
}

static void dev_on_pair_code(const char *code)
{
    fprintf(stdout, "\n*** PAIR CODE: %s ***\n", code);
    fprintf(stdout, "*** Enter this code in the web UI to claim the device ***\n\n");
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
    s_app.config       = cfg;
    s_app.running      = true;
    s_app.send_timer   = AOSL_MPQ_TIMER_INVALID;
    s_app.lock         = aosl_hal_mutex_create();
    if (!s_app.lock) { fprintf(stderr, "[APP] mutex create failed\n"); return -1; }

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- 1. Initialize AOSL ---- */
    aosl_ctor();

    /* ---- 2. Register ALSA audio platform ---- */
    audio_platform_register_alsa_capture();
    audio_platform_register_alsa_playback();

    /* ---- 3. Initialize audio devices ---- */
    const audio_capture_ops_t  *cap_ops = audio_device_get_capture();
    const audio_playback_ops_t *pb_ops  = audio_device_get_playback();
    if (!cap_ops || !pb_ops) { AOSL_LOG_ERR("no audio platform"); goto fail; }

    if (cap_ops->init(&s_app.cap_ctx, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE) < 0)
        { AOSL_LOG_ERR("capture init failed"); goto fail; }
    if (pb_ops->init(&s_app.pb_ctx, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE) < 0)
        { AOSL_LOG_ERR("playback init failed"); cap_ops->destroy(s_app.cap_ctx); goto fail; }

    /* ---- 4. Create ring buffers ---- */
    s_app.cap_ringbuf = ringbuf_create(RINGBUF_SIZE);
    s_app.pb_ringbuf  = ringbuf_create(RINGBUF_SIZE);
    if (!s_app.cap_ringbuf || !s_app.pb_ringbuf)
        { AOSL_LOG_ERR("ringbuf creation failed"); goto fail; }
#if MYBOT_CLOUD_AEC
    s_app.ref_ringbuf = ringbuf_create(RINGBUF_SIZE);
    if (!s_app.ref_ringbuf)
        { AOSL_LOG_ERR("ref ringbuf creation failed"); goto fail; }
    AOSL_LOG_INF("cloud AEC enabled, ref ringbuf created");
#endif

    /* ---- 5. Initialize device state machine ---- */
    device_state_callbacks_t dev_cbs;
    memset(&dev_cbs, 0, sizeof(dev_cbs));
    dev_cbs.on_pair_code           = dev_on_pair_code;
    dev_cbs.on_audio_start         = dev_on_audio_start;
    dev_cbs.on_conversation_start  = dev_on_conversation_start;
    dev_cbs.on_conversation_stop   = dev_on_conversation_stop;
    dev_cbs.on_audio_stop          = dev_on_audio_stop;
    dev_cbs.on_state_changed       = dev_on_state_changed;

    if (device_state_init(cfg->server_base, cfg->device_id,
                          cfg->firmware_ver, cfg->hw_model,
                          &dev_cbs) < 0)
        { AOSL_LOG_ERR("device state init failed"); goto fail; }

    /* ---- 8. Start MPQ main loop ---- */
    AOSL_LOG_INF("starting MPQ main loop...");
    if (aosl_main_start(0, mpq_init, mpq_fini, NULL) < 0)
        { AOSL_LOG_ERR("aosl_main_start failed"); goto fail; }

    /* ---- 9. Set stdin non-blocking for key handling ---- */
    aosl_hal_sk_set_nonblock((aosl_fd_t)0);

    /* ---- 10. Main loop: key handling + state machine ---- */
    fprintf(stdout, "\n"
        "=== mybot ready ===\n"
        "  s - start conversation\n"
        "  q - stop conversation\n"
        "  p - re-pair device\n"
        "  Ctrl+C - exit\n"
        "\n");
    while (s_app.running) {
        char ch;
        if (aosl_hal_sk_read((aosl_fd_t)0, &ch, 1) == 1) {
            switch (ch) {
            case 's':
                fprintf(stdout, "[KEY] s -> start conversation\n");
                device_state_request_start();
                break;
            case 'q':
                fprintf(stdout, "[KEY] q -> stop conversation\n");
                device_state_request_stop();
                break;
            case 'p':
                fprintf(stdout, "[KEY] p -> re-pair\n");
                device_state_request_pair();
                break;
            case '\n':
            case '\r':
                break;
            default:
                fprintf(stdout, "[KEY] '%c' ignored (s=start, q=stop, p=pair)\n", ch);
                break;
            }
        }
        device_state_tick();
        aosl_hal_msleep(100);
    }

    /* ---- 11. Shutdown ---- */
    device_state_request_stop();
    aosl_hal_msleep(500);

    aosl_main_exit_wait();

cleanup:
    AOSL_LOG_INF("cleaning up...");

    /* Step 1: stop RTC session */
    rtc_session_fini();

    /* Step 2: close PCM devices — unblocks capture/playback threads */
    aosl_hal_mutex_lock(s_app.lock);
    if (s_app.cap_ctx) { cap_ops->stop(s_app.cap_ctx); cap_ops->destroy(s_app.cap_ctx); s_app.cap_ctx = NULL; }
    if (s_app.pb_ctx)  { pb_ops->stop(s_app.pb_ctx);  pb_ops->destroy(s_app.pb_ctx);  s_app.pb_ctx  = NULL; }
    aosl_hal_mutex_unlock(s_app.lock);

    /* Step 3: wait for worker threads to exit */
    s_app.running = false;
    aosl_hal_msleep(50);

    if (s_app.cap_thread) { aosl_hal_thread_join(s_app.cap_thread, NULL); s_app.cap_thread = 0; }
    if (s_app.pb_thread)  { aosl_hal_thread_join(s_app.pb_thread, NULL);  s_app.pb_thread  = 0; }

    /* Step 4: destroy ring buffers */
    if (s_app.cap_ringbuf) { ringbuf_destroy(s_app.cap_ringbuf); s_app.cap_ringbuf = NULL; }
    if (s_app.pb_ringbuf)  { ringbuf_destroy(s_app.pb_ringbuf);  s_app.pb_ringbuf  = NULL; }
#if MYBOT_CLOUD_AEC
    if (s_app.ref_ringbuf) { ringbuf_destroy(s_app.ref_ringbuf); s_app.ref_ringbuf = NULL; }
#endif

    aosl_hal_mutex_destroy(s_app.lock);
    aosl_dtor();
    AOSL_LOG_INF("app stopped cleanly");
    return 0;

fail:
    s_app.running = false;
    goto cleanup;
}

void app_stop(void) { s_app.running = false; }
bool app_is_running(void) { return s_app.running; }
