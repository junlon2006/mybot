#include "app.h"
#include "audio/audio_device.h"
#include "ringbuf.h"
#include "protocols/rtc_session.h"

#include "api/aosl.h"
#include "api/aosl_mpq.h"
#include "api/aosl_mpq_timer.h"
#include "api/aosl_log.h"

/* AOSL HAL — cross-platform system interface */
#include <hal/aosl_hal_thread.h>   /* aosl_thread_t, aosl_mutex_t, threads */
#include <hal/aosl_hal_time.h>     /* aosl_hal_msleep */

/* C standard library only — no POSIX system headers */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <signal.h>   /* signal/SIGINT/SIGTERM — TODO: abstract via AOSL */

/* ----------------------------------------------------------
 * Constants
 * ---------------------------------------------------------- */
#define SAMPLE_RATE       16000
#define CHANNELS          1
#define BITS_PER_SAMPLE   16
#define FRAMES_20MS       320       /* 16000 * 20 / 1000 */
#define BYTES_20MS        640       /* 320 * 2 bytes */
#define RINGBUF_SIZE      (BYTES_20MS * 100)  /* ~2 seconds */

/* ----------------------------------------------------------
 * Forward declarations of ALSA platform registration
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
    void                *cap_ctx;
    aosl_thread_t        cap_thread;
    ringbuf_t            cap_ringbuf;

    /* Audio playback */
    void                *pb_ctx;
    aosl_thread_t        pb_thread;
    ringbuf_t            pb_ringbuf;

    /* RTC state */
    volatile bool        rtc_connected;

    /* Timer handle (created in MPQ init callback) */
    aosl_timer_t         send_timer;

    /* Lock for audio device lifecycle */
    aosl_mutex_t         lock;
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
 * Capture worker thread
 *
 * Reads PCM from the platform capture device in a tight loop
 * and writes 20ms frames into the capture ring buffer.
 * ---------------------------------------------------------- */
static void *capture_worker(void *arg)
{
    (void)arg;
    const audio_capture_ops_t *ops = audio_device_get_capture();
    uint8_t pcm[BYTES_20MS];

    AOSL_LOG_INF("capture worker started");

    while (s_app.running) {
        int frames = ops->read(s_app.cap_ctx, pcm, FRAMES_20MS);
        if (frames <= 0) {
            aosl_hal_msleep(5);
            continue;
        }

        int written = ringbuf_write(s_app.cap_ringbuf, (char *)pcm, BYTES_20MS);
        if (written < 0) {
            static int drop_cnt = 0;
            if (++drop_cnt % 100 == 0)
                AOSL_LOG_WRN("capture ringbuf full, dropped %d frames", drop_cnt);
        }
    }

    AOSL_LOG_INF("capture worker stopped");
    return NULL;
}

/* ----------------------------------------------------------
 * Playback worker thread
 *
 * Reads PCM from the playback ring buffer and writes it to
 * the platform playback device.
 * ---------------------------------------------------------- */
static void *playback_worker(void *arg)
{
    (void)arg;
    const audio_playback_ops_t *ops = audio_device_get_playback();
    uint8_t pcm[BYTES_20MS];

    AOSL_LOG_INF("playback worker started");

    while (s_app.running) {
        int avail = ringbuf_get_data_size(s_app.pb_ringbuf);
        if (avail < BYTES_20MS) {
            aosl_hal_msleep(5);
            continue;
        }

        int r = ringbuf_read((char *)pcm, BYTES_20MS, s_app.pb_ringbuf);
        if (r != BYTES_20MS)
            continue;

        ops->write(s_app.pb_ctx, pcm, FRAMES_20MS);
    }

    AOSL_LOG_INF("playback worker stopped");
    return NULL;
}

/* ----------------------------------------------------------
 * RTC audio data callback — called from SDK internal thread.
 * Enqueues remote PCM into the playback ring buffer.
 * ---------------------------------------------------------- */
static void on_remote_audio(uint32_t uid, const void *data, size_t len)
{
    (void)uid;
    if (!s_app.running)
        return;

    int written = ringbuf_write(s_app.pb_ringbuf, (const char *)data, (int)len);
    if (written < 0) {
        static int drop_cnt = 0;
        if (++drop_cnt % 100 == 0)
            AOSL_LOG_WRN("playback ringbuf full, dropped %d remote frames", drop_cnt);
    }
}

/* ----------------------------------------------------------
 * RTC state change callback
 * ---------------------------------------------------------- */
static void on_rtc_state_changed(rtc_state_t state)
{
    AOSL_LOG_INF("rtc state -> %d", (int)state);
    s_app.rtc_connected = (state == RTC_STATE_CONNECTED);
}

/* ----------------------------------------------------------
 * MPQ timer callback — fires every 20ms to send audio
 *
 * Signature: (aosl_timer_t, const aosl_ts_t*, uintptr_t, uintptr_t[])
 * ---------------------------------------------------------- */
static void send_audio_timer(aosl_timer_t timer_id, const aosl_ts_t *now_p,
                             uintptr_t argc, uintptr_t argv[])
{
    (void)timer_id;
    (void)now_p;
    (void)argc;
    (void)argv;

    if (!s_app.rtc_connected)
        return;

    uint8_t pcm[BYTES_20MS];
    int avail = ringbuf_get_data_size(s_app.cap_ringbuf);
    if (avail < BYTES_20MS)
        return;

    int r = ringbuf_read((char *)pcm, BYTES_20MS, s_app.cap_ringbuf);
    if (r == BYTES_20MS)
        rtc_session_send_audio(pcm, BYTES_20MS);
}

/* ----------------------------------------------------------
 * MPQ init callback — runs inside the MPQ thread at startup.
 * Here we create the send timer and join the RTC channel.
 * ---------------------------------------------------------- */
static int mpq_init(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("MPQ loop started");

    /* Create and schedule a 20ms periodic timer for sending audio.
     * This must be done from the MPQ thread. */
    s_app.send_timer = aosl_mpq_set_timer(20, send_audio_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.send_timer)) {
        AOSL_LOG_ERR("failed to create send timer");
    } else {
        AOSL_LOG_INF("send timer created (id=%d)", s_app.send_timer);
    }

    /* Join the RTC channel */
    if (s_app.config) {
        int ret = rtc_session_join(s_app.config->channel,
                                   s_app.config->token,
                                   s_app.config->user);
        if (ret == 0) {
            AOSL_LOG_INF("join channel request sent");
        } else {
            AOSL_LOG_ERR("join channel failed");
        }
    }

    return 0;
}

/* ----------------------------------------------------------
 * MPQ fini callback — runs inside the MPQ thread at shutdown.
 * ---------------------------------------------------------- */
static void mpq_fini(void *arg)
{
    (void)arg;
    AOSL_LOG_INF("MPQ loop stopping");

    /* Kill the send timer */
    if (!aosl_mpq_timer_invalid(s_app.send_timer)) {
        aosl_mpq_kill_timer(s_app.send_timer);
        s_app.send_timer = AOSL_MPQ_TIMER_INVALID;
    }

    /* Leave RTC channel */
    rtc_session_leave();
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int app_start(const app_config_t *cfg)
{
    if (!cfg)
        return -1;

    memset(&s_app, 0, sizeof(s_app));
    s_app.config     = cfg;
    s_app.running    = true;
    s_app.send_timer = AOSL_MPQ_TIMER_INVALID;
    s_app.lock       = NULL;

    /* Create AOSL HAL mutex */
    s_app.lock = aosl_hal_mutex_create();
    if (!s_app.lock) {
        fprintf(stderr, "[APP] mutex create failed\n");
        return -1;
    }

    /* Install signal handler */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- 1. Initialize AOSL ---- */
    aosl_ctor();

    /* ---- 2. Register ALSA platform audio devices ---- */
    audio_platform_register_alsa_capture();
    audio_platform_register_alsa_playback();

    /* ---- 3. Initialize audio devices ---- */
    const audio_capture_ops_t  *cap_ops = audio_device_get_capture();
    const audio_playback_ops_t *pb_ops  = audio_device_get_playback();

    if (!cap_ops || !pb_ops) {
        AOSL_LOG_ERR("no audio platform registered");
        aosl_hal_mutex_destroy(s_app.lock);
        aosl_dtor();
        return -1;
    }

    if (cap_ops->init(&s_app.cap_ctx, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE) < 0) {
        AOSL_LOG_ERR("capture init failed");
        aosl_hal_mutex_destroy(s_app.lock);
        aosl_dtor();
        return -1;
    }

    if (pb_ops->init(&s_app.pb_ctx, SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE) < 0) {
        AOSL_LOG_ERR("playback init failed");
        cap_ops->destroy(s_app.cap_ctx);
        s_app.cap_ctx = NULL;
        aosl_hal_mutex_destroy(s_app.lock);
        aosl_dtor();
        return -1;
    }

    /* ---- 4. Create ring buffers ---- */
    s_app.cap_ringbuf = ringbuf_create(RINGBUF_SIZE);
    s_app.pb_ringbuf  = ringbuf_create(RINGBUF_SIZE);
    if (!s_app.cap_ringbuf || !s_app.pb_ringbuf) {
        AOSL_LOG_ERR("ringbuf creation failed");
        pb_ops->destroy(s_app.pb_ctx);
        cap_ops->destroy(s_app.cap_ctx);
        aosl_hal_mutex_destroy(s_app.lock);
        aosl_dtor();
        return -1;
    }

    /* ---- 5. Start audio devices ---- */
    cap_ops->start(s_app.cap_ctx);
    pb_ops->start(s_app.pb_ctx);

    /* ---- 6. Start capture and playback threads ---- */
    {
        aosl_thread_param_t param;
        param.name       = "cap_worker";
        param.priority   = AOSL_THRD_PRI_NORMAL;
        param.stack_size = 0;  /* system default */

        if (aosl_hal_thread_create(&s_app.cap_thread, &param, capture_worker, NULL) < 0) {
            AOSL_LOG_ERR("failed to create capture thread");
            s_app.running = false;
            goto cleanup;
        }
    }
    {
        aosl_thread_param_t param;
        param.name       = "pb_worker";
        param.priority   = AOSL_THRD_PRI_NORMAL;
        param.stack_size = 0;

        if (aosl_hal_thread_create(&s_app.pb_thread, &param, playback_worker, NULL) < 0) {
            AOSL_LOG_ERR("failed to create playback thread");
            s_app.running = false;
            goto cleanup;
        }
    }

    /* ---- 7. Initialize RTC session ---- */
    rtc_session_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_remote_audio  = on_remote_audio;
    cbs.on_state_changed = on_rtc_state_changed;

    if (rtc_session_init(s_app.config->app_id, &cbs) < 0) {
        AOSL_LOG_ERR("RTC init failed");
        s_app.running = false;
        goto cleanup;
    }

    /* ---- 8. Start the main MPQ event loop ---- */
    AOSL_LOG_INF("starting MPQ main loop...");

    int ret = aosl_main_start(0, mpq_init, mpq_fini, NULL);
    if (ret < 0) {
        AOSL_LOG_ERR("aosl_main_start failed");
        s_app.running = false;
        goto cleanup;
    }

    /* Wait on the main thread until signal */
    while (s_app.running) {
        aosl_hal_msleep(100);
    }

    /* Stop the MPQ loop */
    aosl_main_exit_wait();

cleanup:
    AOSL_LOG_INF("cleaning up...");

    /* Wait for worker threads to stop */
    s_app.running = false;
    if (s_app.cap_thread) {
        aosl_hal_thread_join(s_app.cap_thread, NULL);
        s_app.cap_thread = 0;
    }
    if (s_app.pb_thread) {
        aosl_hal_thread_join(s_app.pb_thread, NULL);
        s_app.pb_thread = 0;
    }

    /* Finalize RTC */
    rtc_session_fini();

    /* Destroy audio devices */
    aosl_hal_mutex_lock(s_app.lock);
    if (s_app.cap_ctx) {
        cap_ops->destroy(s_app.cap_ctx);
        s_app.cap_ctx = NULL;
    }
    if (s_app.pb_ctx) {
        pb_ops->destroy(s_app.pb_ctx);
        s_app.pb_ctx = NULL;
    }
    aosl_hal_mutex_unlock(s_app.lock);

    /* Destroy ring buffers */
    if (s_app.cap_ringbuf) {
        ringbuf_destroy(s_app.cap_ringbuf);
        s_app.cap_ringbuf = NULL;
    }
    if (s_app.pb_ringbuf) {
        ringbuf_destroy(s_app.pb_ringbuf);
        s_app.pb_ringbuf = NULL;
    }

    aosl_hal_mutex_destroy(s_app.lock);

    /* Finalize AOSL */
    aosl_dtor();

    AOSL_LOG_INF("app stopped cleanly");
    return 0;
}

void app_stop(void)
{
    s_app.running = false;
}

bool app_is_running(void)
{
    return s_app.running;
}
