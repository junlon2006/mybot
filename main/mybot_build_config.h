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
 * Default to enabled. Override via compiler flags (-DMYBOT_CLOUD_AEC=0).
 */

#ifndef MYBOT_CLOUD_AEC
#define MYBOT_CLOUD_AEC 1 /* server-side echo cancellation */
#endif

#ifndef MYBOT_AI_QOS
#define MYBOT_AI_QOS 1 /* AI-driven QoS optimization */
#endif

#ifndef MYBOT_FAST_SEND_MULTIPLIER
#define MYBOT_FAST_SEND_MULTIPLIER 3 /* fast-send frames multiplier (1-5) */
#endif

#ifndef MYBOT_SHOW_TRANSCRIPT
#define MYBOT_SHOW_TRANSCRIPT 0 /* real-time transcript in datastream */
#endif

#endif /* MYBOT_BUILD_CONFIG_H_ */
