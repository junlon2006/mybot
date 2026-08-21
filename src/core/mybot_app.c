/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/mybot.h>
#include <mybot/mybot_build_config.h>
#include <mybot/platform/mybot_audio.h>
#include <mybot/platform/mybot_key.h>
#include <mybot/platform/mybot_kv_store.h>
#include <mybot/platform/mybot_lcd.h>
#include <mybot/platform/mybot_wake_words.h>
#include <mybot/platform/mybot_wifi.h>
#include <mybot/platform/mybot_https.h>

#include "mybot_announce_internal.h"
#include "mybot_audio_internal.h"
#include "mybot_https_internal.h"
#include "mybot_key_internal.h"
#include "mybot_kv_store_internal.h"
#include "mybot_lcd_internal.h"
#include "mybot_wake_words_internal.h"
#include "mybot_wifi_internal.h"

#include "mybot_app.h"
#include "mybot_device_lifecycle.h"
#include "mybot_ringbuf.h"
#include "mybot_rtc_session.h"

#include "api/aosl.h"
#include "api/aosl_atomic.h"
#include "api/aosl_mpq.h"
#include "api/aosl_mpq_timer.h"
#include "api/aosl_log.h"

#include <hal/aosl_hal_thread.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------
 * Constants
 * ---------------------------------------------------------- */
#define SAMPLE_RATE 16000 /* Hz */
#define CHANNELS 1
#define BITS_PER_SAMPLE 16
#define BYTES_PER_SAMPLE (BITS_PER_SAMPLE / 8)
#define AUDIO_FRAME_DURATION_MS MYBOT_AUDIO_PTIME_MS
#define AUDIO_FRAME_SAMPLES (SAMPLE_RATE * AUDIO_FRAME_DURATION_MS / 1000)
#define AUDIO_FRAME_BYTES (AUDIO_FRAME_SAMPLES * CHANNELS * BYTES_PER_SAMPLE)
#define AUDIO_RINGBUF_DURATION_MS 2000
#define AUDIO_RINGBUF_SIZE                                                                         \
    (SAMPLE_RATE * AUDIO_RINGBUF_DURATION_MS / 1000 * CHANNELS * BYTES_PER_SAMPLE)
/* Keep this much announcement PCM queued in the playback ring buffer to reduce
 * underrun risk from device-write jitter. */
#define MYBOT_ANNOUNCE_BUFFER_MS 500
#define MYBOT_ANNOUNCE_TARGET_BYTES                                                                \
    (SAMPLE_RATE * MYBOT_ANNOUNCE_BUFFER_MS / 1000 * CHANNELS * BYTES_PER_SAMPLE)
/* Volume change per VOLUME_UP / VOLUME_DOWN key event. */
#define VOLUME_KEY_STEP 10
/* Device state machine poll interval. Must match the 100 ms/tick assumption
 * in mybot_device_lifecycle_tick() (poll_after_seconds * 10 ticks). */
#define STATE_TICK_MS 100
#define MPQ_STACK_SIZE 16384 /* 16 KB stack for aosl_mpq_create threads */

/* ----------------------------------------------------------
 * Global app state
 * ---------------------------------------------------------- */
static struct {
    aosl_atomic_t running;
    aosl_atomic_t state;
    aosl_atomic_t aosl_ref_held;
    bool wifi_active;
    bool kv_store_active;
    bool key_active;
    bool lcd_active;
#if MYBOT_WAKE_WORDS
    bool wake_words_active;
#endif
    mybot_config_t config;
    aosl_mpq_t startup_mpq;

    /* Audio capture */
    void *cap_ctx;
    bool cap_started;
    aosl_mpq_t cap_mpq;     /* capture worker thread (aosl_mpq_create) */
    aosl_timer_t cap_timer; /* drives the capture read loop */
    mybot_ringbuf_t cap_ringbuf;
    uint8_t cap_frame[AUDIO_FRAME_BYTES];

    /* Audio playback */
    void *pb_ctx;
    bool pb_started;
    aosl_mpq_t pb_mpq;     /* playback worker thread (aosl_mpq_create) */
    aosl_timer_t pb_timer; /* drives the playback write loop */
    mybot_ringbuf_t pb_ringbuf;
    aosl_atomic_t announce_clear_pb; /* playback thread: drop buffered announcement tail */
    int16_t pb_pending[AUDIO_FRAME_SAMPLES * CHANNELS];
    int pb_pending_offset;                       /* frames already written */
    int pb_pending_frames;                       /* frames still to write */
    int16_t announce_frame[AUDIO_FRAME_SAMPLES]; /* pairing-code prompt chunk */

#if MYBOT_CLOUD_AEC
    /* AEC reference ringbuf: holds downlink PCM fed to the speaker */
    mybot_ringbuf_t ref_ringbuf;
    int16_t aec_reference_frame[AUDIO_FRAME_SAMPLES];
    int16_t aec_interleaved_frame[AUDIO_FRAME_SAMPLES * 2];
#endif

    /* RTC session state */
    aosl_atomic_t rtc_connected;
    char rtc_app_id[64];
    char rtc_channel[128];
    char rtc_token[512];
    char rtc_uid[64];

    /* Audio sender MPQ: drains captured PCM to RTC at the packet cadence. */
    aosl_mpq_t mpq;
    aosl_timer_t send_timer; /* ptime cadence — send captured PCM to RTC */
    int16_t send_frame[AUDIO_FRAME_SAMPLES * CHANNELS];

    /* Device state machine MPQ: dedicated because mybot_device_lifecycle_tick()
     * performs blocking HTTP polling that must not delay the audio sender or
     * capture/playback workers. */
    aosl_mpq_t state_mpq;
    aosl_timer_t state_timer; /* 100 ms — drive the device state machine */
} s_app;

static void lcd_show_screen(mybot_lcd_screen_t screen) {
    if (s_app.lcd_active && mybot_lcd_show_screen(screen) < 0) {
        AOSL_LOG_WRN("failed to render LCD screen %d", (int)screen);
    }
}

static void lcd_show_pair_code(const char *code) {
    if (s_app.lcd_active && mybot_lcd_show_pair_code(code) < 0) {
        AOSL_LOG_WRN("failed to render LCD pair code");
    }
}

#if MYBOT_WAKE_WORDS
static void on_wake_word(const char *wake_word, void *user_data) {
    (void)user_data;
    AOSL_LOG_NTC("[WAKE WORDS] detected: %s", wake_word ? wake_word : "<unspecified>");
    mybot_app_start_conversation();
}
#endif

/* ----------------------------------------------------------
 * Capture — runs on the capture MPQ thread (cap_mpq).
 * A periodic timer reads one ptime-sized mic frame and feeds cap_ringbuf.
 * ---------------------------------------------------------- */
static void capture_timer(aosl_timer_t id, const aosl_ts_t *now, uintptr_t argc, uintptr_t argv[]) {
    (void)id;
    (void)now;
    (void)argc;
    (void)argv;
    if (!aosl_atomic_read(&s_app.running)) {
        return;
    }

    const mybot_audio_capture_ops_t *ops = mybot_audio_get_capture();

    int frames = ops->read(s_app.cap_ctx, s_app.cap_frame, AUDIO_FRAME_SAMPLES);
    if (frames <= 0) {
        return;
    }
    if (frames > AUDIO_FRAME_SAMPLES) {
        AOSL_LOG_ERR("capture implementation returned invalid frame count: %d > %d", frames,
                     AUDIO_FRAME_SAMPLES);
        return;
    }

#if MYBOT_WAKE_WORDS
    if (s_app.wake_words_active && mybot_get_state() == MYBOT_STATE_READY &&
        mybot_device_lifecycle_get_state() == MYBOT_DEVICE_STATE_RUNTIME) {
        static unsigned int process_error_count;
        if (mybot_wake_words_process(s_app.cap_frame, frames) < 0) {
            process_error_count++;
            if (process_error_count == 1 || process_error_count % 100 == 0) {
                AOSL_LOG_WRN("wake words processing failed (%u errors)", process_error_count);
            }
        } else {
            process_error_count = 0;
        }
    }
#endif

    /* Discard until RTC join succeeds (avoid filling ringbuf with stale data) */
    if (!aosl_atomic_read(&s_app.rtc_connected)) {
        return;
    }

    const int frame_bytes = CHANNELS * BYTES_PER_SAMPLE;
    const int bytes_read = frames * frame_bytes;
    if (mybot_ringbuf_write(s_app.cap_ringbuf, (char *)s_app.cap_frame, bytes_read) < 0) {
        static int dc = 0;
        if (++dc % 100 == 0) {
            AOSL_LOG_WRN("cap ringbuf full, dropped %d", dc);
        }
    }
}

static int cap_mpq_init(void *arg) {
    (void)arg;
    AOSL_LOG_NTC("capture MPQ started");

    s_app.cap_timer = aosl_mpq_set_timer(AUDIO_FRAME_DURATION_MS, capture_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.cap_timer)) {
        AOSL_LOG_ERR("failed to create capture timer");
        return -1;
    }

    return 0;
}

static void cap_mpq_fini(void *arg) {
    (void)arg;
    AOSL_LOG_NTC("capture MPQ stopping");

    if (!aosl_mpq_timer_invalid(s_app.cap_timer)) {
        aosl_mpq_kill_timer(s_app.cap_timer);
        s_app.cap_timer = AOSL_MPQ_TIMER_INVALID;
    }
}

/* ----------------------------------------------------------
 * Playback — runs on the playback MPQ thread (pb_mpq).
 * A periodic timer pulls one ptime-sized frame from pb_ringbuf and
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

    /* Drop any buffered announcement tail when the device leaves pairing.
     * Processed on this (playback) thread so the ring buffer keeps its
     * single-writer/single-reader discipline (no lock). */
    if (aosl_atomic_cmpxchg(&s_app.announce_clear_pb, true, false)) {
        mybot_ringbuf_clear(s_app.pb_ringbuf);
        s_app.pb_pending_offset = 0;
        s_app.pb_pending_frames = 0;
    }

    const mybot_audio_playback_ops_t *ops = mybot_audio_get_playback();

    /* Feed the pairing-code announcement into the playback ring buffer before
     * pulling a frame, so the prompt plays without an RTC session. Top the
     * buffer up to a target level instead of writing one frame per tick:
     * the device write then always has buffered PCM ahead of it and timer
     * jitter cannot cause an underrun. Pairing completes before any RTC
     * session starts, so no cross-thread writer can overlap this feed. */
    while (mybot_announce_is_active() &&
           mybot_ringbuf_get_data_size(s_app.pb_ringbuf) < MYBOT_ANNOUNCE_TARGET_BYTES &&
           mybot_ringbuf_get_free_size(s_app.pb_ringbuf) >= AUDIO_FRAME_BYTES) {
        int frames = mybot_announce_read_pcm(s_app.announce_frame, AUDIO_FRAME_SAMPLES);
        if (frames <= 0) {
            break;
        }
        if (mybot_ringbuf_write(s_app.pb_ringbuf, (const char *)s_app.announce_frame,
                                frames * CHANNELS * BYTES_PER_SAMPLE) < 0) {
            AOSL_LOG_WRN("pb ringbuf full while feeding announcement");
        }
    }

    if (s_app.pb_pending_frames == 0) {
        if (mybot_ringbuf_get_data_size(s_app.pb_ringbuf) < AUDIO_FRAME_BYTES) {
            return;
        }
        if (mybot_ringbuf_read((char *)s_app.pb_pending, AUDIO_FRAME_BYTES, s_app.pb_ringbuf) !=
            AUDIO_FRAME_BYTES) {
            return;
        }
        s_app.pb_pending_offset = 0;
        s_app.pb_pending_frames = AUDIO_FRAME_SAMPLES;

        /* Volume: a registered device volume implementation is the primary control
         * path, so the software gain is applied only as a fallback. Scale
         * before the platform write and before the AEC reference is
         * published, so the reference matches the signal reaching the
         * speaker. */
        if (!mybot_audio_device_volume_is_active()) {
            mybot_audio_apply_media_volume(s_app.pb_pending, AUDIO_FRAME_SAMPLES * CHANNELS);
        }

#if MYBOT_CLOUD_AEC
        /* Publish the downlink PCM as the AEC reference only while an RTC
         * session is connected. Local-only audio (e.g. the pairing-code
         * announcement) must not fill the reference buffer with stale data
         * for the cloud AEC when a conversation starts. */
        if (aosl_atomic_read(&s_app.rtc_connected)) {
            mybot_ringbuf_write(s_app.ref_ringbuf, (const char *)s_app.pb_pending,
                                AUDIO_FRAME_BYTES);
        }
#endif
    }

    const int frame_bytes = CHANNELS * BYTES_PER_SAMPLE;
    int written = ops->write(s_app.pb_ctx,
                             (const char *)s_app.pb_pending + s_app.pb_pending_offset * frame_bytes,
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
        AOSL_LOG_ERR("playback implementation returned invalid frame count: %d > %d", written,
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
    AOSL_LOG_NTC("playback MPQ started");

    s_app.pb_timer = aosl_mpq_set_timer(AUDIO_FRAME_DURATION_MS, playback_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.pb_timer)) {
        AOSL_LOG_ERR("failed to create playback timer");
        return -1;
    }

    return 0;
}

static void pb_mpq_fini(void *arg) {
    (void)arg;
    AOSL_LOG_NTC("playback MPQ stopping");

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

    /* Pairing announcements have priority over RTC downlink audio. While an
     * announcement is active, the playback thread is the sole producer of the
     * playback ring buffer; dropping RTC frames preserves its SPSC contract. */
    if (mybot_announce_is_active()) {
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
    AOSL_LOG_NTC("rtc -> %s", state == MYBOT_RTC_STATE_CONNECTED ? "connected" : "disconnected");

    /* Unexpected RTC drop (connection lost / error): end the conversation.
     * mybot_device_lifecycle_notify_conversation_ended() only acts while the
     * device state machine is IN_CONVERSATION, so a deliberate 'q' stop
     * (state already RUNTIME) is never double-ended. RECONNECTING is transient
     * and is not treated as a drop. SDK errors and connection-loss callbacks
     * may run on SDK threads, so teardown is deferred to state_mpq. */
    if (state == MYBOT_RTC_STATE_DISCONNECTED || state == MYBOT_RTC_STATE_ERROR) {
        mybot_device_lifecycle_notify_conversation_ended();
    }
}

static void on_rtc_token_will_expire(void) {
    if (!aosl_atomic_read(&s_app.running)) {
        return;
    }
    mybot_device_lifecycle_request_rtc_token_renewal();
}

/* ----------------------------------------------------------
 * MPQ timer (ptime cadence) — send captured PCM to RTC
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

    if (mybot_ringbuf_get_data_size(s_app.cap_ringbuf) < AUDIO_FRAME_BYTES) {
        return;
    }

    if (mybot_ringbuf_read((char *)s_app.send_frame, AUDIO_FRAME_BYTES, s_app.cap_ringbuf) ==
        AUDIO_FRAME_BYTES) {
#if MYBOT_CLOUD_AEC
        /* Interleave mic PCM with AEC reference (downlink PCM):
         * output = [mic[0], ref[0], mic[1], ref[1], ...] */
        int16_t *mic = s_app.send_frame;
        size_t samples = AUDIO_FRAME_SAMPLES;

        /* Use silence if insufficient ref data available */
        memset(s_app.aec_reference_frame, 0, sizeof(s_app.aec_reference_frame));
        if (mybot_ringbuf_get_data_size(s_app.ref_ringbuf) >= AUDIO_FRAME_BYTES) {
            mybot_ringbuf_read((char *)s_app.aec_reference_frame, AUDIO_FRAME_BYTES,
                               s_app.ref_ringbuf);
        }

        for (size_t i = 0; i < samples; i++) {
            s_app.aec_interleaved_frame[i * 2] = mic[i];
            s_app.aec_interleaved_frame[i * 2 + 1] = s_app.aec_reference_frame[i];
        }

        mybot_rtc_session_send_audio(s_app.aec_interleaved_frame,
                                     sizeof(s_app.aec_interleaved_frame));
#else
        mybot_rtc_session_send_audio(s_app.send_frame, AUDIO_FRAME_BYTES);
#endif
    }
}

/* ----------------------------------------------------------
 * Device state machine callbacks
 * ---------------------------------------------------------- */

static void dev_on_pair_code(const char *code) {
    AOSL_LOG_NTC("==== PAIR CODE ====");
    AOSL_LOG_NTC("*** PAIR CODE: %s ***", code);
    AOSL_LOG_NTC("*** Enter this pairing code in the console to claim the device ***");
    mybot_state_t app_state = mybot_get_state();
    if (app_state == MYBOT_STATE_STOPPING || app_state == MYBOT_STATE_FAILED ||
        app_state == MYBOT_STATE_WIFI_DISCONNECTED) {
        return;
    }
    lcd_show_pair_code(code);
    mybot_announce_play_pair_code(code);
}

static int dev_on_rtc_token_renewed(const char *token) {
    if (!aosl_atomic_read(&s_app.running)) {
        return -1;
    }

    int ret = mybot_rtc_session_renew_token(token);
    if (ret < 0) {
        return ret;
    }

    snprintf(s_app.rtc_token, sizeof(s_app.rtc_token), "%s", token);
    return 0;
}

static void dev_on_conversation_start(const mybot_conversation_params_t *params) {
    if (!aosl_atomic_read(&s_app.running)) {
        AOSL_LOG_NTC("ignoring conversation start during shutdown");
        mybot_device_lifecycle_notify_conversation_ended();
        return;
    }

    AOSL_LOG_NTC("==== CONVERSATION START ====");
    AOSL_LOG_NTC("  conversation_id: %s", params->conversation_id);
    AOSL_LOG_NTC("  rtc channel    : %s", params->rtc_channel);
    AOSL_LOG_NTC("  rtc uid        : %s", params->rtc_uid);
    AOSL_LOG_NTC("  rtc app_id     : %s", params->rtc_app_id);
    AOSL_LOG_NTC("  rtc token      : %s", params->rtc_token[0] ? "present" : "absent");

    /* Save RTC params and join channel */
    strncpy(s_app.rtc_app_id, params->rtc_app_id, sizeof(s_app.rtc_app_id) - 1);
    strncpy(s_app.rtc_channel, params->rtc_channel, sizeof(s_app.rtc_channel) - 1);
    strncpy(s_app.rtc_token, params->rtc_token, sizeof(s_app.rtc_token) - 1);
    strncpy(s_app.rtc_uid, params->rtc_uid, sizeof(s_app.rtc_uid) - 1);

    /* Init RTC session */
    mybot_rtc_session_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_remote_audio = on_remote_audio;
    cbs.on_token_will_expire = on_rtc_token_will_expire;
    cbs.on_state_changed = on_rtc_state_changed;

    int ret = mybot_rtc_session_init(params->rtc_app_id, &cbs);
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_rtc_session_init failed");
        /* The state machine already moved to IN_CONVERSATION before this
         * callback; roll it back so the device does not stay stuck. */
        mybot_device_lifecycle_notify_conversation_ended();
        return;
    }
    AOSL_LOG_NTC("mybot_rtc_session_init ok");

    /* Join channel with server-assigned string UID */
    AOSL_LOG_NTC("joining RTC as user_account=%s", params->rtc_uid);
    ret = mybot_rtc_session_join(params->rtc_channel, params->rtc_token, params->rtc_uid);
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_rtc_session_join failed");
        mybot_device_lifecycle_notify_conversation_ended();
        return;
    }
    AOSL_LOG_NTC("mybot_rtc_session_join requested, waiting for on_join_channel_success...");
}

static void dev_on_conversation_stop(void) {
    AOSL_LOG_NTC("==== CONVERSATION STOP ====");
    AOSL_LOG_NTC("  channel: %s, uid: %s", s_app.rtc_channel, s_app.rtc_uid);

    /* Stop RTC audio flow first so the capture worker discards new input and
     * send_audio_timer stops sending before the connection is torn down.
     * mybot_rtc_session_leave() is also serialized against sends internally. */
    aosl_atomic_set(&s_app.rtc_connected, false);

    int ret = mybot_rtc_session_leave();
    if (ret < 0) {
        AOSL_LOG_ERR("mybot_rtc_session_leave failed");
    } else {
        AOSL_LOG_NTC("mybot_rtc_session_leave ok");
    }

    AOSL_LOG_NTC("==== CONVERSATION ENDED ====");
}

static void render_device_state(mybot_device_state_t state) {
    mybot_state_t app_state = mybot_get_state();
    if ((state == MYBOT_DEVICE_STATE_IN_CONVERSATION && app_state != MYBOT_STATE_IN_CONVERSATION) ||
        (state != MYBOT_DEVICE_STATE_IN_CONVERSATION && app_state != MYBOT_STATE_READY)) {
        return;
    }

    switch (state) {
    case MYBOT_DEVICE_STATE_UNPROVISIONED:
    case MYBOT_DEVICE_STATE_PAIRING:
        lcd_show_screen(MYBOT_LCD_SCREEN_PAIRING);
        break;
    case MYBOT_DEVICE_STATE_AWAITING_CLAIM:
        /* Keep the pair-code screen rendered by dev_on_pair_code(). */
        break;
    case MYBOT_DEVICE_STATE_RUNTIME:
        lcd_show_screen(MYBOT_LCD_SCREEN_READY);
        break;
    case MYBOT_DEVICE_STATE_IN_CONVERSATION:
        lcd_show_screen(MYBOT_LCD_SCREEN_IN_CONVERSATION);
        break;
    }
}

static void dev_on_state_changed(mybot_device_state_t state) {
    mybot_state_t expected_state = state == MYBOT_DEVICE_STATE_IN_CONVERSATION
                                       ? MYBOT_STATE_READY
                                       : MYBOT_STATE_IN_CONVERSATION;
    mybot_state_t next_state = state == MYBOT_DEVICE_STATE_IN_CONVERSATION
                                   ? MYBOT_STATE_IN_CONVERSATION
                                   : MYBOT_STATE_READY;
    mybot_state_t previous_state =
        (mybot_state_t)aosl_atomic_cmpxchg(&s_app.state, expected_state, next_state);
    if (previous_state != expected_state && previous_state != next_state) {
        return;
    }

    /* Stop the pairing-code voice announcement once the device leaves
     * awaiting_claim (claimed, re-pairing, or offline transitions). The
     * playback thread drops any still-buffered announcement tail so a
     * claim/conversation does not start with stale audio. */
    if (state != MYBOT_DEVICE_STATE_AWAITING_CLAIM) {
        mybot_announce_stop();
        aosl_atomic_set(&s_app.announce_clear_pb, true);
    }

    render_device_state(state);
}

static void key_adjust_volume(int delta) {
    int volume;
    if (mybot_audio_device_volume_is_active()) {
        /* Primary path: drive the real hardware volume. */
        if (mybot_audio_device_get_volume(&volume) == 0) {
            volume += delta;
            if (volume < MYBOT_AUDIO_VOLUME_MIN) {
                volume = MYBOT_AUDIO_VOLUME_MIN;
            } else if (volume > MYBOT_AUDIO_VOLUME_MAX) {
                volume = MYBOT_AUDIO_VOLUME_MAX;
            }
            if (mybot_audio_device_set_volume(volume) == 0) {
                AOSL_LOG_NTC("[KEY] device volume -> %d", volume);
            }
        }
        return;
    }

    /* Fallback path: adjust the SDK software gain. */
    volume = mybot_audio_get_media_volume() + delta;
    if (volume < MYBOT_AUDIO_VOLUME_MIN) {
        volume = MYBOT_AUDIO_VOLUME_MIN;
    } else if (volume > MYBOT_AUDIO_VOLUME_MAX) {
        volume = MYBOT_AUDIO_VOLUME_MAX;
    }
    if (mybot_audio_set_media_volume(volume) == 0) {
        AOSL_LOG_NTC("[KEY] media volume -> %d", volume);
    }
}

static void on_key_event(mybot_key_event_t event, void *user_data) {
    (void)user_data;
    switch (event) {
    case MYBOT_KEY_EVENT_CONVERSATION_START:
        AOSL_LOG_NTC("[KEY] start conversation");
        mybot_app_start_conversation();
        break;
    case MYBOT_KEY_EVENT_CONVERSATION_STOP:
        AOSL_LOG_NTC("[KEY] stop conversation");
        mybot_app_stop_conversation();
        break;
    case MYBOT_KEY_EVENT_PAIR:
        AOSL_LOG_NTC("[KEY] re-pair");
        mybot_app_pair();
        break;
    case MYBOT_KEY_EVENT_VOLUME_UP:
        key_adjust_volume(VOLUME_KEY_STEP);
        break;
    case MYBOT_KEY_EVENT_VOLUME_DOWN:
        key_adjust_volume(-VOLUME_KEY_STEP);
        break;
    case MYBOT_KEY_EVENT_EXIT:
        AOSL_LOG_NTC("[KEY] exit");
        mybot_request_exit();
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
    AOSL_LOG_NTC("state MPQ started");

    s_app.state_timer = aosl_mpq_set_timer(STATE_TICK_MS, state_tick_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.state_timer)) {
        AOSL_LOG_ERR("failed to create state timer");
        return -1;
    }

    return 0;
}

static void state_mpq_fini(void *arg) {
    (void)arg;
    AOSL_LOG_NTC("state MPQ stopping");

    if (!aosl_mpq_timer_invalid(s_app.state_timer)) {
        aosl_mpq_kill_timer(s_app.state_timer);
        s_app.state_timer = AOSL_MPQ_TIMER_INVALID;
    }

    mybot_device_lifecycle_shutdown();
}

/* ----------------------------------------------------------
 * Audio sender MPQ init — runs inside its MPQ thread at startup
 * ---------------------------------------------------------- */
static int mpq_init(void *arg) {
    (void)arg;
    AOSL_LOG_NTC("MPQ loop started");

    s_app.send_timer = aosl_mpq_set_timer(AUDIO_FRAME_DURATION_MS, send_audio_timer, NULL, 0);
    if (aosl_mpq_timer_invalid(s_app.send_timer)) {
        AOSL_LOG_ERR("failed to create send timer");
        return -1;
    }

    return 0;
}

/* ----------------------------------------------------------
 * Audio sender MPQ fini — runs inside its MPQ thread at shutdown
 * ---------------------------------------------------------- */
static void mpq_fini(void *arg) {
    (void)arg;
    AOSL_LOG_NTC("MPQ loop stopping");

    if (!aosl_mpq_timer_invalid(s_app.send_timer)) {
        aosl_mpq_kill_timer(s_app.send_timer);
        s_app.send_timer = AOSL_MPQ_TIMER_INVALID;
    }

    mybot_rtc_session_leave();
}

/* ----------------------------------------------------------
 * Service teardown
 *
 * cleanup_services() releases everything start_services() may have
 * initialized. It is idempotent and is called both from the
 * start_services() failure path (so partial startup leaks nothing even if
 * the host never calls mybot_stop()) and from mybot_stop().
 *
 * Ring buffers are intentionally NOT destroyed here: on_remote_audio() (an
 * RTC SDK callback thread) writes pb_ringbuf, and only mybot_rtc_session_fini()
 * guarantees the callback queue has drained. Callers must run
 * destroy_audio_ringbufs() after RTC finalization; in the start_services()
 * failure path RTC is never initialized, so it is safe there as well.
 * ---------------------------------------------------------- */
static void cleanup_services(void) {
    if (s_app.key_active) {
        mybot_key_deinit();
        s_app.key_active = false;
    }

    /* Wait for any in-flight HTTP operation, prevent further state-machine
     * actions, and close an active server conversation before RTC/audio
     * resources are dismantled. */
    if (!aosl_mpq_invalid(s_app.state_mpq)) {
        aosl_mpq_destroy_wait(s_app.state_mpq);
        s_app.state_mpq = AOSL_MPQ_INVALID;
    }

    /* The audio sender MPQ fini callback kills the send timer and leaves the RTC
     * channel. Must happen before mybot_rtc_session_fini(): agora_rtc_fini()
     * tears down the SDK queues and releases the SDK's own AOSL reference;
     * the application reference remains held until mybot_stop() finishes. */
    if (!aosl_mpq_invalid(s_app.mpq)) {
        aosl_mpq_destroy_wait(s_app.mpq);
        s_app.mpq = AOSL_MPQ_INVALID;
    }

    /* Capture/playback worker MPQs: aosl_mpq_destroy_wait() destroys the
     * queue and joins its thread in one call, replacing the thread-HAL join
     * that is not portable. */
    if (!aosl_mpq_invalid(s_app.cap_mpq)) {
        aosl_mpq_destroy_wait(s_app.cap_mpq);
        s_app.cap_mpq = AOSL_MPQ_INVALID;
    }
    if (!aosl_mpq_invalid(s_app.pb_mpq)) {
        aosl_mpq_destroy_wait(s_app.pb_mpq);
        s_app.pb_mpq = AOSL_MPQ_INVALID;
    }

    /* The playback worker has exited, so no announcement feed can be in flight. */
    mybot_announce_deinit();

#if MYBOT_WAKE_WORDS
    /* The capture MPQ has exited, so process() cannot race with destroy(). */
    if (s_app.wake_words_active) {
        mybot_wake_words_deinit();
        s_app.wake_words_active = false;
    }
#endif

    if (s_app.kv_store_active) {
        mybot_kv_store_deinit();
        s_app.kv_store_active = false;
    }

    /* Audio devices: their worker MPQs have exited, so no read/write can
     * race with stop. */
    const mybot_audio_capture_ops_t *cap_ops = mybot_audio_get_capture();
    const mybot_audio_playback_ops_t *pb_ops = mybot_audio_get_playback();
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
    if (cap_ops && s_app.cap_ctx) {
        cap_ops->destroy(s_app.cap_ctx);
        s_app.cap_ctx = NULL;
    }
    if (pb_ops && s_app.pb_ctx) {
        pb_ops->destroy(s_app.pb_ctx);
        s_app.pb_ctx = NULL;
    }
    /* Release the optional device volume implementation after the audio devices. */
    mybot_audio_device_volume_deinit();
}

static void destroy_audio_ringbufs(void) {
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
}

static int start_services(void) {
    const mybot_config_t *cfg = &s_app.config;

    /* ---- 1. Initialize local storage and key input services. ---- */
    if (mybot_kv_store_init() < 0) {
        AOSL_LOG_ERR("kv store init failed");
        goto fail;
    }
    s_app.kv_store_active = true;

    if (mybot_key_init(on_key_event, NULL) < 0) {
        AOSL_LOG_ERR("key service init failed");
        goto fail;
    }
    s_app.key_active = true;

    /* Optional pairing-code voice announcement. A failure here only disables
     * the prompt; the device keeps working. */
    if (mybot_announce_init() < 0) {
        AOSL_LOG_WRN("announce init failed, pairing voice prompt disabled");
    }

    /* ---- 2. Initialize audio devices via the registered platform ops ----
     * The platform implementation (e.g. ALSA on Linux) must have registered itself
     * through mybot_audio_register_*() before mybot_start() is called. */
    const mybot_audio_capture_ops_t *cap_ops = mybot_audio_get_capture();
    const mybot_audio_playback_ops_t *pb_ops = mybot_audio_get_playback();
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

    /* Optional real-device volume implementation. Its init failure is non-fatal: the
     * missing control only disables device volume, software media volume and
     * playback keep working. */
    if (mybot_audio_device_volume_init() < 0) {
        AOSL_LOG_WRN(
            "device volume implementation unavailable, real-device volume control disabled");
    }

#if MYBOT_WAKE_WORDS
    if (!mybot_wake_words_is_registered()) {
        AOSL_LOG_ERR("wake words enabled but no local ASR implementation is registered");
        goto fail;
    }
    if (mybot_wake_words_init(SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE, on_wake_word, NULL) < 0) {
        AOSL_LOG_ERR("wake words implementation init failed");
        goto fail;
    }
    s_app.wake_words_active = true;
#endif

    /* ---- 3. Create ring buffers ---- */
    s_app.cap_ringbuf = mybot_ringbuf_create(AUDIO_RINGBUF_SIZE);
    s_app.pb_ringbuf = mybot_ringbuf_create(AUDIO_RINGBUF_SIZE);
    if (!s_app.cap_ringbuf || !s_app.pb_ringbuf) {
        AOSL_LOG_ERR("ringbuf creation failed");
        goto fail;
    }
#if MYBOT_CLOUD_AEC
    s_app.ref_ringbuf = mybot_ringbuf_create(AUDIO_RINGBUF_SIZE);
    if (!s_app.ref_ringbuf) {
        AOSL_LOG_ERR("ref ringbuf creation failed");
        goto fail;
    }
    AOSL_LOG_NTC("cloud AEC enabled, ref ringbuf created");
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
     * platform. The per-MPQ timer (cap_timer / pb_timer) drives bounded
     * platform I/O on its own thread, isolated from the audio sender. */
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
    dev_cbs.on_rtc_token_renewed = dev_on_rtc_token_renewed;
    dev_cbs.on_state_changed = dev_on_state_changed;

    if (mybot_device_lifecycle_init(cfg->server_base, cfg->device_id, cfg->firmware_ver,
                                    cfg->hw_model, &dev_cbs) < 0) {
        AOSL_LOG_ERR("device state init failed");
        goto fail;
    }

    /* ---- 7. Create the audio sender MPQ ----
     * Use aosl_mpq_create() instead of aosl_main_start(): the latter
     * registers an atexit() hook that re-runs aosl_main_exit_wait() after
     * main() returns, which can run after the final aosl_dtor() has released AOSL.
     * Creating the queue explicitly keeps teardown fully in our control. */
    AOSL_LOG_NTC("starting MPQ loop...");
    s_app.mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 1000, "mybot_mpq", mpq_init,
                                mpq_fini, NULL);
    if (aosl_mpq_invalid(s_app.mpq)) {
        AOSL_LOG_ERR("aosl_mpq_create failed");
        goto fail;
    }

    /* ---- 8. Create the device-state MPQ ----
     * mybot_device_lifecycle_tick() performs blocking HTTP polling, so it runs
     * on a dedicated thread that cannot delay the audio workers. */
    s_app.state_mpq = aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 1000, "state_mpq",
                                      state_mpq_init, state_mpq_fini, NULL);
    if (aosl_mpq_invalid(s_app.state_mpq)) {
        AOSL_LOG_ERR("state_mpq create failed");
        goto fail;
    }

    return 0;

fail:
    /* Release everything initialized so far. RTC is never initialized before
     * the state machine runs (the last step), so destroying the ring buffers
     * here cannot race with RTC callbacks. */
    cleanup_services();
    destroy_audio_ringbufs();
    return -1;
}

static void handle_wifi_event(const aosl_ts_t *queued_ts, aosl_refobj_t robj, uintptr_t argc,
                              uintptr_t argv[]) {
    (void)queued_ts;
    (void)robj;
    if (argc != 1) {
        return;
    }

    mybot_state_t app_state = mybot_get_state();
    if (app_state == MYBOT_STATE_STOPPING || app_state == MYBOT_STATE_FAILED) {
        return;
    }

    mybot_wifi_event_t wifi_event = (mybot_wifi_event_t)argv[0];
    if (wifi_event == MYBOT_WIFI_EVENT_FAILED && app_state == MYBOT_STATE_WIFI_PROVISIONING) {
        if (aosl_atomic_cmpxchg(&s_app.state, MYBOT_STATE_WIFI_PROVISIONING, MYBOT_STATE_FAILED) ==
            MYBOT_STATE_WIFI_PROVISIONING) {
            lcd_show_screen(MYBOT_LCD_SCREEN_FAILED);
            AOSL_LOG_ERR("wifi provisioning failed");
            aosl_atomic_set(&s_app.running, false);
        }
        return;
    }

    if (wifi_event == MYBOT_WIFI_EVENT_STA_DISCONNECTED) {
        if (app_state == MYBOT_STATE_READY || app_state == MYBOT_STATE_IN_CONVERSATION) {
            mybot_device_lifecycle_set_network_available(false);
            aosl_atomic_set(&s_app.state, MYBOT_STATE_WIFI_DISCONNECTED);
            AOSL_LOG_WRN("Wi-Fi disconnected; pausing device-service activity");
        } else if (app_state == MYBOT_STATE_WIFI_DISCONNECTED) {
            return;
        }
        lcd_show_screen(MYBOT_LCD_SCREEN_WIFI_DISCONNECTED);
        return;
    }

    if (wifi_event == MYBOT_WIFI_EVENT_FAILED && app_state != MYBOT_STATE_WIFI_PROVISIONING) {
        if (app_state == MYBOT_STATE_WIFI_DISCONNECTED) {
            return;
        }
        mybot_device_lifecycle_set_network_available(false);
        aosl_atomic_set(&s_app.state, MYBOT_STATE_WIFI_DISCONNECTED);
        lcd_show_screen(MYBOT_LCD_SCREEN_WIFI_DISCONNECTED);
        AOSL_LOG_WRN("Wi-Fi failed at runtime; pausing device-service activity");
        return;
    }

    if (wifi_event == MYBOT_WIFI_EVENT_STA_CONNECTED &&
        app_state == MYBOT_STATE_WIFI_DISCONNECTED) {
        mybot_device_lifecycle_set_network_available(true);
        aosl_atomic_set(&s_app.state, MYBOT_STATE_READY);
        AOSL_LOG_NTC("Wi-Fi reconnected; resuming device-service activity");
        render_device_state(mybot_device_lifecycle_get_state());
        return;
    }

    if (wifi_event != MYBOT_WIFI_EVENT_STA_CONNECTED ||
        aosl_atomic_cmpxchg(&s_app.state, MYBOT_STATE_WIFI_PROVISIONING,
                            MYBOT_STATE_STARTING_SERVICES) != MYBOT_STATE_WIFI_PROVISIONING) {
        return;
    }

    lcd_show_screen(MYBOT_LCD_SCREEN_STARTING_SERVICES);
    if (start_services() < 0) {
        if (aosl_atomic_read(&s_app.state) != MYBOT_STATE_STOPPING) {
            lcd_show_screen(MYBOT_LCD_SCREEN_FAILED);
            aosl_atomic_set(&s_app.state, MYBOT_STATE_FAILED);
            aosl_atomic_set(&s_app.running, false);
        }
        return;
    }

    if (aosl_atomic_cmpxchg(&s_app.state, MYBOT_STATE_STARTING_SERVICES, MYBOT_STATE_READY) ==
        MYBOT_STATE_STARTING_SERVICES) {
        render_device_state(mybot_device_lifecycle_get_state());
    }
}

static void on_wifi_event(mybot_wifi_event_t event, void *user_data) {
    (void)user_data;
    mybot_state_t app_state = mybot_get_state();
    if (app_state == MYBOT_STATE_STOPPING || app_state == MYBOT_STATE_FAILED ||
        app_state == MYBOT_STATE_STOPPED) {
        return;
    }

    if (aosl_mpq_queue(s_app.startup_mpq, AOSL_MPQ_INVALID, AOSL_REF_INVALID, "handle_wifi_event",
                       handle_wifi_event, 1, (uintptr_t)event) < 0) {
        AOSL_LOG_ERR("failed to queue wifi event");
        if (aosl_atomic_cmpxchg(&s_app.state, app_state, MYBOT_STATE_FAILED) == app_state) {
            lcd_show_screen(MYBOT_LCD_SCREEN_FAILED);
            aosl_atomic_set(&s_app.running, false);
        }
    }
}

/* ----------------------------------------------------------
 * Public API
 * ---------------------------------------------------------- */

int mybot_start(const mybot_config_t *cfg) {
    if (aosl_atomic_read(&s_app.aosl_ref_held)) {
        AOSL_LOG_ERR("mybot_start: application is already active");
        return -1;
    }
    if (!cfg || !memchr(cfg->server_base, '\0', sizeof(cfg->server_base)) ||
        !memchr(cfg->device_id, '\0', sizeof(cfg->device_id)) ||
        !memchr(cfg->firmware_ver, '\0', sizeof(cfg->firmware_ver)) ||
        !memchr(cfg->hw_model, '\0', sizeof(cfg->hw_model)) || !cfg->server_base[0] ||
        !cfg->device_id[0]) {
        AOSL_LOG_ERR("mybot_start: invalid configuration (NULL or unterminated string fields; "
                     "server_base and device_id must be non-empty)");
        return -1;
    }

    bool use_https = strncmp(cfg->server_base, "https://", 8) == 0;
    bool use_http = strncmp(cfg->server_base, "http://", 7) == 0;
#if MYBOT_ENABLE_HTTPS
    if (use_https && !mybot_https_is_registered()) {
        AOSL_LOG_ERR("mybot_start: https:// requires a registered TLS transport "
                     "(call mybot_https_register before mybot_start)");
        return -1;
    }
#else
    if (use_https) {
        AOSL_LOG_ERR("mybot_start: https:// unsupported in this build (MYBOT_ENABLE_HTTPS=OFF)");
        return -1;
    }
#endif
#if !MYBOT_ALLOW_INSECURE_HTTP
    if (use_http) {
        AOSL_LOG_ERR("mybot_start: http:// rejected (build with MYBOT_ALLOW_INSECURE_HTTP=ON "
                     "for isolated development only)");
        return -1;
    }
#endif
    if (!use_https && !use_http) {
        AOSL_LOG_ERR("mybot_start: unsupported URL scheme in server_base "
                     "(expected https:// or http://)");
        return -1;
    }

    memset(&s_app, 0, sizeof(s_app));
    memcpy(&s_app.config, cfg, sizeof(s_app.config));
    aosl_atomic_set(&s_app.running, true);
    aosl_atomic_set(&s_app.state, MYBOT_STATE_STOPPED);
    s_app.startup_mpq = AOSL_MPQ_INVALID;
    s_app.mpq = AOSL_MPQ_INVALID;
    s_app.send_timer = AOSL_MPQ_TIMER_INVALID;
    s_app.cap_mpq = AOSL_MPQ_INVALID;
    s_app.cap_timer = AOSL_MPQ_TIMER_INVALID;
    s_app.pb_mpq = AOSL_MPQ_INVALID;
    s_app.pb_timer = AOSL_MPQ_TIMER_INVALID;
    s_app.state_mpq = AOSL_MPQ_INVALID;
    s_app.state_timer = AOSL_MPQ_TIMER_INVALID;

    aosl_ctor();
    aosl_atomic_set(&s_app.aosl_ref_held, true);

    if (mybot_lcd_is_registered()) {
        if (mybot_lcd_init() < 0) {
            AOSL_LOG_ERR("LCD init failed");
            goto fail;
        }
        s_app.lcd_active = true;
        lcd_show_screen(MYBOT_LCD_SCREEN_STARTING);
    }

    s_app.startup_mpq =
        aosl_mpq_create(AOSL_THRD_PRI_NORMAL, MPQ_STACK_SIZE, 32, "startup_mpq", NULL, NULL, NULL);
    if (aosl_mpq_invalid(s_app.startup_mpq)) {
        AOSL_LOG_ERR("startup_mpq create failed");
        goto fail;
    }

    aosl_atomic_set(&s_app.state, MYBOT_STATE_WIFI_PROVISIONING);
    lcd_show_screen(MYBOT_LCD_SCREEN_WIFI_PROVISIONING);
    if (mybot_wifi_init(s_app.config.device_id, on_wifi_event, NULL) < 0) {
        AOSL_LOG_ERR("wifi provisioning init failed");
        goto fail;
    }
    s_app.wifi_active = true;
    AOSL_LOG_NTC("wifi provisioning started");
    return 0;

fail:
    lcd_show_screen(MYBOT_LCD_SCREEN_FAILED);
    aosl_atomic_set(&s_app.state, MYBOT_STATE_FAILED);
    mybot_stop();
    return -1;
}

bool mybot_is_running(void) {
    return aosl_atomic_read(&s_app.running) != 0;
}

mybot_state_t mybot_get_state(void) {
    return (mybot_state_t)aosl_atomic_read(&s_app.state);
}

void mybot_request_exit(void) {
    aosl_atomic_set(&s_app.running, false);
}

void mybot_app_start_conversation(void) {
    if (mybot_get_state() != MYBOT_STATE_READY) {
        return;
    }
    mybot_device_lifecycle_request_start();
}

void mybot_app_stop_conversation(void) {
    if (mybot_get_state() != MYBOT_STATE_IN_CONVERSATION) {
        return;
    }
    mybot_device_lifecycle_request_stop();
}

void mybot_app_pair(void) {
    mybot_state_t state = mybot_get_state();
    if (state != MYBOT_STATE_READY && state != MYBOT_STATE_IN_CONVERSATION) {
        return;
    }
    mybot_device_lifecycle_request_pair();
}

void mybot_stop(void) {
    /* Keep stop idempotent without touching AOSL after a previous stop. */
    if (!aosl_atomic_read(&s_app.aosl_ref_held)) {
        return;
    }

    AOSL_LOG_NTC("stopping app...");

    /* ---- 1. Block further startup transitions and signal workers to stop ----
     * Set BEFORE any AOSL/audio teardown so the MPQ timer callbacks return
     * early. Platform read/write callbacks are required to bound blocking, so
     * each worker can exit even when its device makes no progress. */
    mybot_state_t previous_state = mybot_get_state();
    aosl_atomic_set(&s_app.state, MYBOT_STATE_STOPPING);
    aosl_atomic_set(&s_app.running, false);
    if (previous_state != MYBOT_STATE_FAILED) {
        lcd_show_screen(MYBOT_LCD_SCREEN_STOPPING);
    }

    /* ---- 2. Join the startup queue ----
     * A Wi-Fi event may already be running start_services() on this queue.
     * Joining it before teardown serializes partial startup with cleanup. */
    if (!aosl_mpq_invalid(s_app.startup_mpq)) {
        aosl_mpq_destroy_wait(s_app.startup_mpq);
        s_app.startup_mpq = AOSL_MPQ_INVALID;
    }

    /* ---- 3. Tear down all services started by start_services() ----
     * Idempotent: the same function runs on the start_services() failure
     * path, so stopping after a failed startup releases nothing twice. */
    cleanup_services();

    /* Reassert the terminal screen after all device workflow callbacks have drained. */
    lcd_show_screen(previous_state == MYBOT_STATE_FAILED ? MYBOT_LCD_SCREEN_FAILED
                                                         : MYBOT_LCD_SCREEN_STOPPING);

    /* ---- 4. Stop Wi-Fi after all network users have exited. ---- */
    if (s_app.wifi_active) {
        mybot_wifi_deinit();
        s_app.wifi_active = false;
    }

    /* ---- 5. Stop the LCD after all workflow event sources have exited. ---- */
    if (s_app.lcd_active) {
        mybot_lcd_deinit();
        s_app.lcd_active = false;
    }

    /* ---- 6. Finalize RTC ----
     * The SDK waits for its callback queue before returning, so no callback can
     * access pb_ringbuf after this point. */
    mybot_rtc_session_fini();

    /* ---- 7. Destroy ring buffers ----
     * The AOSL HAL allocator is independent of the AOSL global lifecycle. */
    destroy_audio_ringbufs();

    /* ---- 8. Release the application's AOSL reference last ----
     * Every aosl_ctor() has one matching aosl_dtor(). The RTC SDK owns a
     * separate reference through agora_rtc_init/fini(), so its fini call never
     * replaces this release. No AOSL API may be used after this call. */
    AOSL_LOG_NTC("app stopped cleanly");
    aosl_atomic_set(&s_app.state, MYBOT_STATE_STOPPED);
    if (aosl_atomic_read(&s_app.aosl_ref_held)) {
        aosl_dtor();
        aosl_atomic_set(&s_app.aosl_ref_held, false);
    }
}
