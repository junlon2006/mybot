#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agora_rtc_api.h"
#include "log.h"
#include "utility.h"

#define DEFAULT_WAIT_SECONDS 10
#define DEFAULT_INTERVAL_MS 1000
#define MAX_CHANNELS 20

typedef struct {
  const char *app_id;
  const char *token;
  const char *license;
  const char *log_dir;
  const char *user_id;
  const char *channel;
  const char *message;
  const char *custom_type;
  int count;
  int interval_ms;
  int publish_delay_seconds;
  int wait_seconds;
  bool subscribe;
} app_config_t;

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_logged_in;
static volatile sig_atomic_t g_subscribe_results;
static volatile sig_atomic_t g_subscribe_failures;

static void on_signal(int signal_number) {
  (void)signal_number;
  g_stop = 1;
}

static void on_rtm_event(const char *user_id, uint32_t event_id, uint32_t event_code) {
  LOGS("EVENT user=%s event=%u code=%u", user_id, event_id, event_code);
  if (event_id == RTM_EVENT_TYPE_LOGIN && event_code == ERR_RTM_OK) {
    g_logged_in = 1;
  } else if (event_id == RTM_EVENT_TYPE_EXIT || event_id == RTM_EVENT_TYPE_KICKOFF) {
    g_logged_in = 0;
  }
}

static void on_rtm_subscribe_result(const char *channel_name, rtm_err_code_e error_code) {
  LOGS("SUBSCRIBE_RESULT channel=%s code=%d", channel_name, error_code);
  if (error_code != ERR_RTM_OK) g_subscribe_failures++;
  g_subscribe_results++;
}

static void on_rtm_channel_data(const char *channel_name, const char *user_id, const void *message,
                            size_t length, const char *custom_type) {
  LOGS("RECV channel=%s sender=%s length=%zu custom_type=%s", channel_name, user_id, length,
       custom_type == NULL ? "" : custom_type);
  printf("RECV_DATA ");
  fwrite(message, 1, length, stdout);
  printf("\n");
  fflush(stdout);
}

static void on_rtm_publish_result(const char *channel_name, uint32_t message_id,
                              rtm_msg_state_e state) {
  static const char *states[] = {"INIT", "RECEIVED", "UNREACHABLE", "TIMEOUT"};
  const char *state_name = state >= RTM_MSG_STATE_INIT && state <= RTM_MSG_STATE_TIMEOUT
                               ? states[state]
                               : "UNKNOWN";
  LOGS("PUBLISH_RESULT channel=%s msg_id=%u state=%s", channel_name, message_id, state_name);
}

static void usage(const char *program) {
  LOGS("Usage: %s -i <app_id> -u <user_id> -c <channel[,channel...]> [options]", program);
  LOGS("  -t, --token <token>          RTM token");
  LOGS("  -l, --license <license>      SDK license");
  LOGS("  -L, --log-dir <path>         SDK log directory");
  LOGS("  -m, --message <text>         Binary payload to publish; omit to receive only");
  LOGS("  -T, --custom-type <type>     Custom message type");
  LOGS("  -n, --count <count>          Publish count, default 1");
  LOGS("  -I, --interval-ms <ms>       Publish interval, default %d", DEFAULT_INTERVAL_MS);
  LOGS("  -d, --publish-delay <sec>    Delay after subscriptions before publishing");
  LOGS("  -w, --wait-seconds <seconds> Receive wait after publishing, default %d",
       DEFAULT_WAIT_SECONDS);
  LOGS("  -N, --no-subscribe           Publish without subscribing");
  LOGS("Example: %s -i APP_ID -u rtsa -c test -m hello -w 10", program);
}

static int parse_args(app_config_t *config, int argc, char **argv) {
  static const struct option options[] = {
      {"app-id", required_argument, NULL, 'i'},
      {"token", required_argument, NULL, 't'},
      {"license", required_argument, NULL, 'l'},
      {"log-dir", required_argument, NULL, 'L'},
      {"user-id", required_argument, NULL, 'u'},
      {"channel", required_argument, NULL, 'c'},
      {"message", required_argument, NULL, 'm'},
      {"custom-type", required_argument, NULL, 'T'},
      {"count", required_argument, NULL, 'n'},
      {"interval-ms", required_argument, NULL, 'I'},
      {"publish-delay", required_argument, NULL, 'd'},
      {"wait-seconds", required_argument, NULL, 'w'},
      {"no-subscribe", no_argument, NULL, 'N'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };
  int option;

  while ((option = getopt_long(argc, argv, "hi:t:l:L:u:c:m:T:n:I:d:w:N", options, NULL)) != -1) {
    switch (option) {
      case 'i': config->app_id = optarg; break;
      case 't': config->token = optarg; break;
      case 'l': config->license = optarg; break;
      case 'L': config->log_dir = optarg; break;
      case 'u': config->user_id = optarg; break;
      case 'c': config->channel = optarg; break;
      case 'm': config->message = optarg; break;
      case 'T': config->custom_type = optarg; break;
      case 'n': config->count = atoi(optarg); break;
      case 'I': config->interval_ms = atoi(optarg); break;
      case 'd': config->publish_delay_seconds = atoi(optarg); break;
      case 'w': config->wait_seconds = atoi(optarg); break;
      case 'N': config->subscribe = false; break;
      default: return -1;
    }
  }
  return config->app_id != NULL && config->user_id != NULL && config->channel != NULL &&
                 config->count > 0 && config->interval_ms >= 0 &&
                 config->publish_delay_seconds >= 0 && config->wait_seconds >= 0
             ? 0
             : -1;
}

static bool wait_flag(volatile sig_atomic_t *flag, int timeout_seconds) {
  int elapsed_ms = 0;
  while (!g_stop && !*flag && elapsed_ms < timeout_seconds * 1000) {
    util_sleep_ms(10);
    elapsed_ms += 10;
  }
  return *flag != 0;
}

static bool wait_count(volatile sig_atomic_t *value, int expected, int timeout_seconds) {
  int elapsed_ms = 0;
  while (!g_stop && *value < expected && elapsed_ms < timeout_seconds * 1000) {
    util_sleep_ms(10);
    elapsed_ms += 10;
  }
  return *value >= expected;
}

int main(int argc, char **argv) {
  app_config_t config = {
      .log_dir = "io.agora.rtm_channel",
      .license = "",
      .count = 1,
      .interval_ms = DEFAULT_INTERVAL_MS,
      .wait_seconds = DEFAULT_WAIT_SECONDS,
      .subscribe = true,
  };
  agora_rtc_event_handler_t rtc_handler = {0};
  agora_rtm_handler_t rtm_handler = {
      .on_rtm_event = on_rtm_event,
      .on_rtm_subscribe_result = on_rtm_subscribe_result,
      .on_rtm_subscribe_data = on_rtm_channel_data,
      .on_rtm_publish_result = on_rtm_publish_result,
  };
  rtc_service_option_t service_option = {0};
  char *channel_storage;
  char *channels[MAX_CHANNELS];
  char *save_ptr = NULL;
  char *channel;
  int channel_count = 0;
  int result;

  if (parse_args(&config, argc, argv) != 0) {
    usage(argv[0]);
    return 2;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  channel_storage = strdup(config.channel);
  if (channel_storage == NULL) return 1;
  channel = strtok_r(channel_storage, ",", &save_ptr);
  while (channel != NULL && channel_count < MAX_CHANNELS) {
    channels[channel_count++] = channel;
    channel = strtok_r(NULL, ",", &save_ptr);
  }
  if (channel_count == 0 || channel != NULL) {
    LOGE("CHANNEL_LIST invalid or exceeds %d channels", MAX_CHANNELS);
    free(channel_storage);
    return 2;
  }

  service_option.area_code = AREA_CODE_GLOB;
  service_option.log_cfg.log_path = config.log_dir;
  service_option.log_cfg.log_level = RTC_LOG_INFO;
  snprintf(service_option.license_value, sizeof(service_option.license_value), "%s", config.license);

  result = agora_rtc_init(config.app_id, &rtc_handler, &service_option);
  if (result != 0) {
    LOGE("INIT ret=%d error=%s", result, agora_rtc_err_2_str(result));
    free(channel_storage);
    return 1;
  }
  result = agora_rtm_login(config.user_id, config.token, &rtm_handler);
  LOGS("LOGIN user=%s ret=%d", config.user_id, result);
  if (result != 0 || !wait_flag(&g_logged_in, 10)) {
    LOGE("LOGIN result=timeout_or_rejected");
    agora_rtc_fini();
    free(channel_storage);
    return 1;
  }

  if (config.subscribe) {
    for (int i = 0; i < channel_count; ++i) {
      result = agora_rtm_subscribe(channels[i]);
      LOGS("SUBSCRIBE channel=%s ret=%d", channels[i], result);
      if (result != 0) {
        g_subscribe_failures++;
        g_subscribe_results++;
      }
    }
    if (!wait_count(&g_subscribe_results, channel_count, 10) || g_subscribe_failures != 0) {
      LOGE("SUBSCRIBE result=timeout_or_rejected channels=%s results=%d failures=%d",
           config.channel, g_subscribe_results, g_subscribe_failures);
      agora_rtm_logout();
      agora_rtc_fini();
      free(channel_storage);
      return 1;
    }
  }

  if (config.message != NULL) {
    size_t length = strlen(config.message);
    uint32_t message_id = 0;
    if (config.publish_delay_seconds > 0) {
      util_sleep_ms(config.publish_delay_seconds * 1000);
    }
    for (int i = 0; i < config.count && !g_stop; ++i) {
      for (int j = 0; j < channel_count && !g_stop; ++j) {
        ++message_id;
        result = agora_rtm_publish(channels[j], config.message, length, message_id,
                                   config.custom_type);
        LOGS("PUBLISH channel=%s msg_id=%u length=%zu ret=%d", channels[j], message_id, length,
             result);
        if (result != 0) {
          LOGE("PUBLISH error=%s", agora_rtc_err_2_str(result));
        }
        if (i + 1 < config.count || j + 1 < channel_count) util_sleep_ms(config.interval_ms);
      }
    }
  }

  for (int i = 0; i < config.wait_seconds * 10 && !g_stop; ++i) util_sleep_ms(100);
  if (config.subscribe) {
    for (int i = 0; i < channel_count; ++i) {
      result = agora_rtm_unsubscribe(channels[i]);
      LOGS("UNSUBSCRIBE channel=%s ret=%d", channels[i], result);
    }
  }
  agora_rtm_logout();
  agora_rtc_fini();
  free(channel_storage);
  return 0;
}
