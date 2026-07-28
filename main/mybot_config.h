#ifndef MYBOT_CONFIG_H_
#define MYBOT_CONFIG_H_

/*
 * Feature flags for voice chat capabilities.
 * Default to enabled. Override via compiler flags (-DMYBOT_CLOUD_AEC=0).
 */

#ifndef MYBOT_CLOUD_AEC
#define MYBOT_CLOUD_AEC                1   /* server-side echo cancellation */
#endif

#ifndef MYBOT_AI_QOS
#define MYBOT_AI_QOS                   1   /* AI-driven QoS optimization */
#endif

#ifndef MYBOT_FAST_SEND_MULTIPLIER
#define MYBOT_FAST_SEND_MULTIPLIER     3   /* fast-send frames multiplier (1-5) */
#endif

#ifndef MYBOT_SHOW_TRANSCRIPT
#define MYBOT_SHOW_TRANSCRIPT          0   /* real-time transcript in datastream */
#endif

#endif /* MYBOT_CONFIG_H_ */
