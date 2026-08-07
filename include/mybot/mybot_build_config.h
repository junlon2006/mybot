#ifndef MYBOT_BUILD_CONFIG_H_
#define MYBOT_BUILD_CONFIG_H_

/* Audio packetization interval in milliseconds. Agora supports 20, 40, or 60 ms. */
#ifndef MYBOT_AUDIO_PTIME_MS
#define MYBOT_AUDIO_PTIME_MS 60
#endif

#if MYBOT_AUDIO_PTIME_MS != 20 && MYBOT_AUDIO_PTIME_MS != 40 && MYBOT_AUDIO_PTIME_MS != 60
#error "MYBOT_AUDIO_PTIME_MS must be 20, 40, or 60"
#endif

/*
 * Feature flags for voice chat capabilities.
 * Override defaults via compiler flags (for example, -DMYBOT_CLOUD_AEC=0).
 */

#ifndef MYBOT_CLOUD_AEC
#define MYBOT_CLOUD_AEC 1 /* server-side echo cancellation */
#endif

#if MYBOT_CLOUD_AEC != 0 && MYBOT_CLOUD_AEC != 1
#error "MYBOT_CLOUD_AEC must be 0 or 1"
#endif

#ifndef MYBOT_WAKE_WORDS
#define MYBOT_WAKE_WORDS 0 /* local ASR wake-word detection */
#endif

#if MYBOT_WAKE_WORDS != 0 && MYBOT_WAKE_WORDS != 1
#error "MYBOT_WAKE_WORDS must be 0 or 1"
#endif

#ifndef MYBOT_AI_QOS
#define MYBOT_AI_QOS 1 /* AI-driven QoS optimization */
#endif

#if MYBOT_AI_QOS != 0 && MYBOT_AI_QOS != 1
#error "MYBOT_AI_QOS must be 0 or 1"
#endif

#ifndef MYBOT_FAST_SEND_MULTIPLIER
#define MYBOT_FAST_SEND_MULTIPLIER 3 /* fast-send frames multiplier (1-5) */
#endif

#if MYBOT_FAST_SEND_MULTIPLIER < 1 || MYBOT_FAST_SEND_MULTIPLIER > 5
#error "MYBOT_FAST_SEND_MULTIPLIER must be from 1 through 5"
#endif

#ifndef MYBOT_SHOW_TRANSCRIPT
#define MYBOT_SHOW_TRANSCRIPT 0 /* real-time transcript in datastream */
#endif

#if MYBOT_SHOW_TRANSCRIPT != 0 && MYBOT_SHOW_TRANSCRIPT != 1
#error "MYBOT_SHOW_TRANSCRIPT must be 0 or 1"
#endif

#endif /* MYBOT_BUILD_CONFIG_H_ */
