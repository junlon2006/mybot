/*************************************************************
 * File  :  hello_rtsa.c
 * Module:  Agora SD-RTN SDK RTC C API demo application.
 *
 * This is a part of the Agora RTC Service SDK.
 * Copyright (C) 2020 Agora IO
 * All rights reserved.
 *
 *************************************************************/

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>

#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "pthread.h"

#include "agora_rtc_api.h"
#include "utility.h"
#include "pacer.h"
#include "log.h"

#define TAG_APP "[app]"
#define TAG_API "[api]"
#define TAG_EVENT "[event]"

#define DEFAULT_RTM_SEND_FILENAME "hello_rtm_file"
#define DEFAULT_RTM_RECV_FILENAME "recv_rtm_file"
#define DEFAULT_RTM_SEND_SIZE (10 * 1024)
#define MAX_RTM_DATA_SIZE (31*1024)
#define MIN_RTM_SEND_INTERVAL_MS (17)
#define DEF_RTM_SEND_KBPS (10 * DEFAULT_RTM_SEND_SIZE * 8 / 1024) // send 10 msg

#define RTM_MSG_STATE_INVALID  (-1)

#define MY_MSG_QUEUE_MAX (60)
#define MY_MSG_ID_INFO   (0)
#define MY_MSG_ID_RESET  ((uint32_t)-1)


typedef struct {
  uint32_t msg_id;
  uint16_t msg_len;
  char msg_data[0];
} my_message_t;

#define my_message_size(msg) (sizeof(my_message_t) + (msg)->msg_len)

typedef struct {
  my_message_t *msg;
  rtm_msg_state_e state;
  uint64_t sent_ts;
} message_cached_t;

typedef struct {
  pthread_mutex_t ctx_mtx;
  uint32_t msg_id_inc;
  uint32_t msg_cnt;
  uint32_t min_msg_id;
  uint32_t max_msg_id;
  message_cached_t msg_slot[MY_MSG_QUEUE_MAX];
} message_snd_ctx_t;

typedef struct {
  uint32_t expect_msg_id;
} message_rcv_ctx_t;

typedef struct {
  const char *p_sdk_log_dir;

  const char *p_appid;
  const char *p_token;
	const char *p_license;
  const char *p_peer_uid;
  const char *p_rtm_uid;

  uint32_t send_size;
  uint32_t send_kbps;

  bool send_enable_flag;
  const char *send_file_path;

  bool recv_dump_flag;
  const char *recv_file_path;
} app_config_t;

typedef struct {
  app_config_t config;

  FILE *send_file_fp;
  FILE *recv_file_fp;
  message_snd_ctx_t send_file_ctx;
  message_rcv_ctx_t recv_file_ctx;
  uint32_t send_file_size;
  uint32_t recv_file_size;
  bool send_file_size_delivered;

  int32_t b_stop_flag;
  int32_t b_login_success_flag;
} app_t;

static app_t g_app_instance = {
	.config =
			{
					.p_sdk_log_dir = "io.agora.rtc_sdk",
					.p_appid = "",
					.p_token = NULL,
					.p_license = "",
					.p_peer_uid = "",
					.p_rtm_uid = "",

					.send_size = DEFAULT_RTM_SEND_SIZE,
					.send_kbps = DEF_RTM_SEND_KBPS,
					.send_enable_flag = 0,
					.send_file_path = DEFAULT_RTM_SEND_FILENAME,

					.recv_dump_flag = 1,
					.recv_file_path = DEFAULT_RTM_RECV_FILENAME,
			},

	.b_stop_flag = 0,
	.b_login_success_flag = 0,

  .send_file_ctx = {0},
  .recv_file_ctx = {0},
};

static void *s_sender_pacer = NULL;

app_t *app_get_instance(void)
{
  return &g_app_instance;
}

static void app_signal_handler(int32_t sig)
{
  app_t *p_app = app_get_instance();
  switch (sig) {
  case SIGQUIT:
  case SIGABRT:
  case SIGINT:
    p_app->b_stop_flag = 1;
    break;
  default:
    LOGW("no handler, sig=%d", sig);
  }
}

void app_print_usage(int32_t argc, char **argv)
{
  LOGS("\nUsage: %s [OPTION]", argv[0]);
  LOGS(" -h, --help               : show help info");
  LOGS(" -i, --appId              : application id, either appId OR token MUST be set");
  LOGS(" -t, --token              : token for authentication");
  LOGS(" -l, --license            : license value MUST be set when release");
  LOGS(" -L, --log dir            : sdk log dir");
  LOGS(" -u, --rtmUid             : rtm uid, default is './%s'", "user");
  LOGS(" -p, --peerUid            : peer uid, default is './%s'", "peer");
  LOGS(" -s, --sendFlag           : send enable flag");
  LOGS(" -f, --sendFile           : file to send, default is './%s'", DEFAULT_RTM_SEND_FILENAME);
  LOGS(" -F, --recvFile           : file received to save, default is './%s'", DEFAULT_RTM_RECV_FILENAME);
  LOGS(" -S, --sendSize           : send data length, default is '%d'", DEFAULT_RTM_SEND_SIZE);
  LOGS(" -K, --sendKbps           : send data kbps, default is '%d'", DEF_RTM_SEND_KBPS);
  LOGS("\nExample:");
  LOGS("    %s --appId xxx [--token xxx] --rtmUid xxx --peerUid xxx --role xxx --sendFile xxx --recvFile xxx --sendSize xxx --sendFreq xxx",
       argv[0]);
}

void app_print_config(app_config_t *p_config)
{
  LOGS("------------------config------------------");
  LOGS("appid         =   %s", p_config->p_appid);
  LOGS("token         =   %s", p_config->p_token);
  LOGS("license       =   %s", p_config->p_license);
  LOGS("logdir        =   %s", p_config->p_sdk_log_dir);
  LOGS("self_id       =   %s", p_config->p_rtm_uid);
  LOGS("peer_id       =   %s", p_config->p_peer_uid);
  LOGS("sendFile      =   %s", p_config->send_file_path);
  LOGS("recvFile      =   %s", p_config->recv_file_path);
  LOGS("sendSize      =   %u (B)", p_config->send_size);
  LOGS("sendKbps      =   %u", p_config->send_kbps);
  LOGS("------------------config----------------");
}

int32_t app_parse_args(app_config_t *p_config, int32_t argc, char **argv)
{
  const char *av_short_option = "hi:t:l:L:u:p:sf:F:S:K:";
  const struct option av_long_option[] = { { "help", 0, NULL, 'h' },
                                           { "appId", 1, NULL, 'i' },
                                           { "token", 1, NULL, 't' },
                                           { "license", 1, NULL, 'l' },
                                           { "log", 1, NULL, 'L' },
                                           { "rtmUid", 1, NULL, 'u' },
                                           { "peerUid", 1, NULL, 'p' },
                                           { "sendFlag", 0, NULL, 's' },
                                           { "sendFile", 1, NULL, 'f' },
                                           { "recvFile", 1, NULL, 'F' },
                                           { "sendSize", 1, NULL, 'S' },
                                           { "sendKbps", 1, NULL, 'K' },
                                           { 0, 0, 0, 0 } };

  int32_t ch = -1;
  int32_t optidx = 0;
  int32_t rval = 0;

  while (1) {
    optidx++;
    ch = getopt_long(argc, argv, av_short_option, av_long_option, NULL);
    if (ch == -1) {
      break;
    }

    switch (ch) {
    case 'h': {
      rval = -1;
      goto EXIT;
    } break;
    case 'i': {
      p_config->p_appid = optarg;
    } break;
    case 't': {
      p_config->p_token = optarg;
    } break;
    case 'l': {
      p_config->p_license = optarg;
    } break;
    case 'L': {
      p_config->p_sdk_log_dir = optarg;
    } break;
    case 'u': {
      p_config->p_rtm_uid = optarg;
    } break;
    case 'p': {
      p_config->p_peer_uid = optarg;
    } break;
    case 's': {
      p_config->send_enable_flag = true;
    } break;
    case 'f': {
      p_config->send_file_path = optarg;
    } break;
    case 'F': {
      p_config->recv_file_path = optarg;
    } break;
    case 'S': {
      p_config->send_size = strtol(optarg, NULL, 10);
    } break;
    case 'K': {
      p_config->send_kbps = strtol(optarg, NULL, 10);
    } break;
    default: {
      rval = -1;
      LOGS("%s parse cmd param: %s error.", TAG_APP, argv[optidx]);
      goto EXIT;
    }
    }
  }

  app_print_config(p_config);

  // check key parameters
  if (strcmp(p_config->p_appid, "") == 0) {
    rval = -1;
    LOGE("%s appid MUST be provided", TAG_APP);
    goto EXIT;
  }

  if (!p_config->send_file_path || strcmp(p_config->send_file_path, "") == 0 ||
      !p_config->recv_file_path || strcmp(p_config->recv_file_path, "") == 0) {
    rval = -1;
    LOGE("%s invalid rtm send recv path", TAG_APP);
    goto EXIT;
  }

  if (!p_config->p_rtm_uid || strcmp(p_config->p_rtm_uid, "") == 0) {
    rval = -1;
    LOGE("%s invalid rtm uid", TAG_APP);
    goto EXIT;
  }

  if (!p_config->p_peer_uid || strcmp(p_config->p_peer_uid, "") == 0) {
    rval = -1;
    LOGE("%s invalid peer uid", TAG_APP);
    goto EXIT;
  }

  if (p_config->send_size > MAX_RTM_DATA_SIZE || p_config->send_size <= 0) {
    p_config->send_size = DEFAULT_RTM_SEND_SIZE;
    LOGW("RTM send size should between 1 and 31kb, set to default %d", DEFAULT_RTM_SEND_SIZE);
  }

  uint32_t min_kbps = p_config->send_size * 8 / 1024;
  uint32_t max_kbps = p_config->send_size * 8 * 60 / 1024;
  uint32_t def_kbps = p_config->send_size * 8 * 10 / 1024;
  if (p_config->send_kbps < min_kbps || p_config->send_kbps > max_kbps) {
    LOGD("kbps=%u should in [%u, %u] when send_size=%u, set default %u",
         p_config->send_kbps, min_kbps, max_kbps, p_config->send_size, def_kbps);
    p_config->send_kbps = def_kbps;
  }

EXIT:
  return rval;
}

static void message_snd_ctx_init(message_snd_ctx_t *ctx)
{
  memset(ctx, 0, sizeof(message_snd_ctx_t));
  for (int i = 0; i < MY_MSG_QUEUE_MAX; i++) {
    ctx->msg_slot[i].state = RTM_MSG_STATE_INVALID;
  }
  pthread_mutex_init(&ctx->ctx_mtx, NULL);
}

static void message_snd_ctx_fini(message_snd_ctx_t *ctx)
{
  for (int i = 0; i < MY_MSG_QUEUE_MAX; i++) {
    if (ctx->msg_slot[i].msg) {
      free(ctx->msg_slot[i].msg);
      ctx->msg_slot[i].msg = NULL;
    }
  }
  pthread_mutex_destroy(&ctx->ctx_mtx);
}

static void message_snd_ctx_reset(message_snd_ctx_t *ctx)
{
  ctx->msg_id_inc = 0;
  ctx->msg_cnt = 0;
  ctx->min_msg_id =0;
  ctx->max_msg_id = 0;
  for (int i = 0; i < MY_MSG_QUEUE_MAX; i++) {
    ctx->msg_slot[i].state = RTM_MSG_STATE_INVALID;
    if (ctx->msg_slot[i].msg) {
      free(ctx->msg_slot[i].msg);
      ctx->msg_slot[i].msg = NULL;
    }
  }
}

static void message_rcv_ctx_init(message_rcv_ctx_t *ctx)
{
  ctx->expect_msg_id = 1;
}

static void message_rcv_ctx_fini(message_rcv_ctx_t *ctx)
{
  ctx->expect_msg_id = 1;
}

static void message_rcv_ctx_reset(message_rcv_ctx_t *ctx)
{
  ctx->expect_msg_id = 1;
}

static int32_t app_init(app_t *p_app)
{
  int32_t rval = 0;

  signal(SIGQUIT, app_signal_handler);
  signal(SIGABRT, app_signal_handler);
  signal(SIGINT, app_signal_handler);

  app_config_t *p_config = &p_app->config;

  if (p_config->send_enable_flag) {
    p_app->send_file_fp = fopen(p_app->config.send_file_path, "r");
    if (p_app->send_file_fp == NULL) {
      LOGE("%s parser open send file:%s failed", TAG_APP, p_config->send_file_path);
      rval = -1;
      goto EXIT;
    }
    fseek(p_app->send_file_fp, 0, SEEK_END);
    p_app->send_file_size = ftell(p_app->send_file_fp);
    fseek(p_app->send_file_fp, 0, SEEK_SET);
    if (p_app->send_file_size == 0) {
      LOGE("empty send file");
      goto EXIT;
    }
  }

  if (p_config->recv_dump_flag) {
    p_app->recv_file_fp = fopen(p_config->recv_file_path, "w+");
    if (p_app->recv_file_fp == NULL) {
      rval = -1;
      LOGE("%s open audio dump file: %s failed", TAG_APP, p_config->recv_file_path);
      goto EXIT;
    }
  }

  message_snd_ctx_init(&p_app->send_file_ctx);
  message_rcv_ctx_init(&p_app->recv_file_ctx);

EXIT:
  return rval;
}

static void app_deinit(app_t *p_app)
{
  if (p_app->send_file_fp) {
    fclose(p_app->send_file_fp);
    p_app->send_file_fp = NULL;
  }

  if (p_app->recv_file_fp) {
    fclose(p_app->recv_file_fp);
    p_app->recv_file_fp = NULL;
  }

  message_snd_ctx_fini(&p_app->send_file_ctx);
  message_rcv_ctx_fini(&p_app->recv_file_ctx);

  p_app->b_login_success_flag = 0;
  p_app->b_stop_flag = 0;

  if (s_sender_pacer) {
    pacer_destroy(s_sender_pacer);
  }
}

static my_message_t* message_get(app_t *p_app)
{
  message_snd_ctx_t *ctx = &p_app->send_file_ctx;
  message_cached_t *cached = NULL;
  my_message_t *msg = NULL;

  pthread_mutex_lock(&ctx->ctx_mtx);

  uint32_t id = ctx->msg_id_inc + 1;
  cached = &ctx->msg_slot[id % MY_MSG_QUEUE_MAX];
  if (cached->state != RTM_MSG_STATE_INVALID) {
    // It's already occupied
    goto __tag_out;
  }
  cached->state = RTM_MSG_STATE_INIT;
  cached->sent_ts = 0;

  msg = malloc(sizeof(my_message_t) + p_app->config.send_size);
  cached->msg = msg;
  msg->msg_id = id;
  msg->msg_len = 0;

  if (ctx->msg_cnt == 0) {
    ctx->min_msg_id = id;
    ctx->max_msg_id = id;
  } else {
    if (seq_uint32_before(id, ctx->min_msg_id)) {
      ctx->min_msg_id = id;
    }
    if (seq_uint32_after(id, ctx->max_msg_id)) {
      ctx->max_msg_id = id;
    }
  }
  ++ctx->msg_id_inc;
  ++ctx->msg_cnt;

__tag_out:
  pthread_mutex_unlock(&ctx->ctx_mtx);
  return msg;
}

static void message_put(app_t *p_app, uint32_t msg_id)
{
  message_snd_ctx_t *ctx = &p_app->send_file_ctx;
  message_cached_t *cached = NULL;
  message_cached_t *slot = NULL;

  pthread_mutex_lock(&ctx->ctx_mtx);

  if (ctx->msg_cnt == 0) {
    goto __tag_out;
  }
  if (!seq_uint32_in_range(msg_id, ctx->min_msg_id, ctx->max_msg_id)) {
    goto __tag_out;
  }

  cached = &ctx->msg_slot[msg_id % MY_MSG_QUEUE_MAX];
  cached->sent_ts = 0;
  cached->state = RTM_MSG_STATE_INVALID;
  ctx->msg_cnt--;
  free(cached->msg);
  cached->msg = NULL;

  if (ctx->msg_cnt == 0) {
    ctx->min_msg_id = 0;
    ctx->max_msg_id = 0;
    goto __tag_out;
  }

  if (msg_id == ctx->min_msg_id) {
    for (uint32_t id = msg_id+1; seq_uint32_before_eq(id, ctx->max_msg_id); id++) {
      slot = &ctx->msg_slot[id % MY_MSG_QUEUE_MAX];
      if (slot->state != RTM_MSG_STATE_INVALID) {
        ctx->min_msg_id = id;
        goto __tag_out;
      }
    }
  }

  if (msg_id == ctx->max_msg_id) {
    for (uint32_t id = msg_id-1; seq_uint32_after_eq(id, ctx->min_msg_id); id--) {
      slot = &ctx->msg_slot[id % MY_MSG_QUEUE_MAX];
      if (slot->state != RTM_MSG_STATE_INVALID) {
        ctx->max_msg_id = id;
        goto __tag_out;
      }
    }
  }

__tag_out:
  pthread_mutex_unlock(&ctx->ctx_mtx);
}

static void message_update(app_t *p_app, uint32_t msg_id, rtm_msg_state_e state)
{
  message_snd_ctx_t *ctx = &p_app->send_file_ctx;
  message_cached_t *cached = NULL;

  pthread_mutex_lock(&ctx->ctx_mtx);

  if (ctx->msg_cnt == 0) {
    goto __tag_out;
  }
  if (!seq_uint32_in_range(msg_id, ctx->min_msg_id, ctx->max_msg_id)) {
    goto __tag_out;
  }

  cached = &ctx->msg_slot[msg_id % MY_MSG_QUEUE_MAX];

  if (cached->state == RTM_MSG_STATE_INVALID) {
    goto __tag_out;
  }
  if (cached->state == RTM_MSG_STATE_RECEIVED) {
    // ignore state result when had received
    goto __tag_out;
  }
  cached->state = state;

__tag_out:
  pthread_mutex_unlock(&ctx->ctx_mtx);
}

static void notify_send_file_reset(app_t *p_app)
{
  LOGD("------------notify file reset------------");

  message_rcv_ctx_reset(&p_app->recv_file_ctx);

  static my_message_t msg = {0};
  msg.msg_id = MY_MSG_ID_RESET;
  msg.msg_len = 0;

  (void)agora_rtc_send_rtm_data(p_app->config.p_peer_uid, &msg, my_message_size(&msg), msg.msg_id, NULL);
}

static void notify_send_file_info(app_t *p_app)
{
  LOGD("------------notify file info------------");

  static char buf[1024] = {0};
  static my_message_t *msg = (my_message_t*)buf;
  msg->msg_id = MY_MSG_ID_INFO;
  msg->msg_len = 4 + strlen(p_app->config.send_file_path);
  *((uint32_t*)msg->msg_data) = p_app->send_file_size;
  strcpy(msg->msg_data+4, p_app->config.send_file_path);

  (void)agora_rtc_send_rtm_data(p_app->config.p_peer_uid, msg, my_message_size(msg), msg->msg_id, NULL);
}

static void send_file_reset(app_t *p_app)
{
  message_snd_ctx_t *ctx = &p_app->send_file_ctx;

  LOGD("---------------send_file_reset------------");

  pthread_mutex_lock(&ctx->ctx_mtx);

  fseek(p_app->send_file_fp, 0, SEEK_SET);
  message_snd_ctx_reset(&p_app->send_file_ctx);
  p_app->send_file_size_delivered = false;

  pthread_mutex_unlock(&ctx->ctx_mtx);
}

static bool is_message_expected(app_t *p_app, my_message_t *msg)
{
  message_rcv_ctx_t *ctx = &p_app->recv_file_ctx;
  if (msg->msg_id != ctx->expect_msg_id) {
    if (seq_uint32_before(msg->msg_id, ctx->expect_msg_id)) {
      // ignore old msg
      return false;
    }
    LOGE("not expected msg, msg_id=%u expect_msg_id=%u", msg->msg_id, ctx->expect_msg_id);
    notify_send_file_reset(p_app);
    return false;
  }

  ctx->expect_msg_id++;
  return true;
}

static void __on_rtm_event(const char *user_id, uint32_t event_id, uint32_t event_code)
{
  app_t *p_app = app_get_instance();
  LOGD("<<<<<<<<<<<<<<<<<< user_id[%s] event id[%u], event code[%u] >>>>>>>>>>>>>>>>>>", user_id, event_id, event_code);
  if (event_id == 0 && event_code == 0) {
    p_app->b_login_success_flag = 1;
  } else {
    p_app->b_login_success_flag = 0;
  }
}

static void __on_rtm_data(const char *user_id, const void *data, size_t data_len, const char *custom_type)
{
  app_t *p_app = app_get_instance();
  app_config_t *p_config = &p_app->config;
  static float progress = 0;
  my_message_t *msg = (my_message_t *)data;

  if (p_config->send_enable_flag && msg->msg_id == MY_MSG_ID_RESET) {
    send_file_reset(p_app);
    return;
  }
  if (msg->msg_id == MY_MSG_ID_INFO) {
    LOGD("----------------------got file info---------------------------");
    uint32_t file_size = *(uint32_t*)msg->msg_data;
    char*    file_path = msg->msg_data + 4;
    p_app->recv_file_size = file_size;
    LOGD("file_path=%s file_size=%u (KB)", file_path, file_size/1024);
    return;
  }

  if (p_app->recv_file_size) {
    static uint32_t msg_len = 0;
    msg_len = msg->msg_len > msg_len ? msg->msg_len : msg_len;
    progress = (float)(msg->msg_id * msg_len) * 100.0 / (float)p_app->recv_file_size;
    progress = progress > 100 ? 100 : progress;
  }

  LOGD("[peer-%s] msg_id=%u, msg_len=%u progress=%.3f", user_id, msg->msg_id, msg->msg_len, progress);

  if (p_app->config.recv_dump_flag && p_app->recv_file_fp) {
    if (!is_message_expected(p_app, msg)) {
      return;
    }
    if (fwrite(msg->msg_data, 1, msg->msg_len, p_app->recv_file_fp) != msg->msg_len) {
      LOGE("write error");
      return;
    }
  }

  if (progress == 100) {
    message_rcv_ctx_reset(&p_app->recv_file_ctx);
    LOGD("Received file successfully, reset to initial state");
  }
}

static void __on_rtm_send_data_res(const char *rtm_uid, uint32_t msg_id, rtm_msg_state_e state)
{
  app_t *p_app = app_get_instance();
  bool is_received = (state == RTM_MSG_STATE_RECEIVED);

  if (msg_id == MY_MSG_ID_RESET) {
    if (is_received) {
      LOGD("[peer-%s] file reset had sent successfully", rtm_uid);
    }
    return;
  }
  if (msg_id == MY_MSG_ID_INFO) {
    if (is_received) {
      p_app->send_file_size_delivered = true;
      LOGD("[peer-%s] file info had delivered successfully", rtm_uid);
    }
    return;
  }

  LOGD("[peer-%s] result callback: msg_id=%u, state=%u", rtm_uid, msg_id, state);

  if (is_received) {
    message_put(p_app, msg_id);
  } else {
    message_update(p_app, msg_id, state);
  }
}

static agora_rtc_event_handler_t event_handler = {
  .on_error = NULL,
};

static agora_rtm_handler_t rtm_handler = {
  .on_rtm_data = __on_rtm_data,
  .on_rtm_event = __on_rtm_event,
  .on_rtm_send_data_result = __on_rtm_send_data_res,
};

static int check_resend(app_t *p_app)
{
  int ret = 0;
  int cnt = 0;
  message_snd_ctx_t *ctx = &p_app->send_file_ctx;
  const char *peer_id = p_app->config.p_peer_uid;
  message_cached_t *cached = NULL;
  my_message_t *msg = NULL;

  pthread_mutex_lock(&ctx->ctx_mtx);

  if (ctx->msg_cnt) {
    uint64_t now = util_get_time_ms();
    uint64_t diff = 0;
    uint32_t id = 0;
    for(id = ctx->min_msg_id; seq_uint32_before_eq(id, ctx->max_msg_id) && cnt < 3; id++) {
      cached = &ctx->msg_slot[id % MY_MSG_QUEUE_MAX];
      diff   = now - cached->sent_ts;
      msg    = cached->msg;
      if (cached->state == RTM_MSG_STATE_INVALID) {
        continue;
      }
      if (cached->sent_ts == 0 || (cached->state != RTM_MSG_STATE_RECEIVED && diff >= 10000)) {
        cnt++;
        ret = agora_rtc_send_rtm_data(peer_id, msg, my_message_size(msg), msg->msg_id, NULL);
        if (ret < 0) {
          LOGE("re-send msg_id=%u failed, ret=%d/%s", msg->msg_id, ret, agora_rtc_err_2_str(ret));
          goto __tag_out;
        } else {
          cached->sent_ts = now;
        }
      } // if timeout
    } // for
  } // if snd_cnt

__tag_out:
  pthread_mutex_unlock(&ctx->ctx_mtx);
  return cnt;
}

static void send_file(app_t *p_app)
{
  int ret = 0;
  float progress = 0;
  message_snd_ctx_t *ctx = &p_app->send_file_ctx;
  const char *peer_id = p_app->config.p_peer_uid;
  message_cached_t *cached = NULL;
  my_message_t *msg = NULL;

  // 0. 检查文件信息是否交付到对端
  if (!p_app->send_file_size_delivered) {
    notify_send_file_info(p_app);
  }

  // 1. 将发送缓冲区中超时的消息重新发送, 一次最多暂定发送3个消息
  if (check_resend(p_app)) {
    return;
  }

  // 2. 发送新的消息
  // 2.0 检查文件尾巴
  if (feof(p_app->send_file_fp)) {
    if (ctx->msg_cnt == 0) {
      LOGD("Have sent file successfully, exit");
      p_app->b_stop_flag = 1;
    }
    return;
  }

  // 2.1 获取一个新的message
  msg = message_get(p_app);
  if (!msg) {
    LOGD("can't send msg, send buf is full");
    return;
  }
  cached = &ctx->msg_slot[msg->msg_id % MY_MSG_QUEUE_MAX];

  // 2.2 填充 message data
  int rsize = 0;
  uint32_t max_data_size = p_app->config.send_size;
  rsize = fread(msg->msg_data, 1, max_data_size, p_app->send_file_fp);
  if (rsize <= 0) {
    LOGD("read end of file, fp=%p", p_app->send_file_fp);
    return;
  }
  msg->msg_len = rsize;

  // 2.3 发送这个新的 message
  ret = agora_rtc_send_rtm_data(peer_id, msg, my_message_size(msg), msg->msg_id, NULL);
  if (ret < 0) {
    LOGE("send msg_id=%u failed, ret=%d/%s", msg->msg_id, ret, agora_rtc_err_2_str(ret));
    return;
  } else {
    cached->sent_ts = util_get_time_ms();

    progress = (float)(msg->msg_id * p_app->config.send_size) * 100.0 / (float)p_app->send_file_size;
    progress = progress > 100 ? 100 : progress;
    LOGD("send progress %.3f", progress);

    return;
  }
}

void send_file_proc(app_t *p_app)
{
  if (!s_sender_pacer) {
    uint32_t s_send_bps = p_app->config.send_kbps * 1000;
    uint32_t s_send_fps = (s_send_bps) / (p_app->config.send_size * 8);
    uint32_t s_send_interval_us = 1000 * 1000 / s_send_fps;
    s_sender_pacer = pacer_create(s_send_interval_us, 0);
  }

  if (!p_app->b_login_success_flag) {
    util_sleep_ms(100);
    return;
  }

  if (is_time_to_send_audio(s_sender_pacer)) {
    send_file(p_app);
  }

  wait_before_next_send(s_sender_pacer);
}

int32_t main(int32_t argc, char **argv)
{
  LOGS("%s Welcome to RTSA-RTM SDK v%s", TAG_APP, agora_rtc_get_version());

  app_t *p_app = app_get_instance();
  app_config_t *p_config = &p_app->config;

  // 0. app parse args
  int32_t rval = app_parse_args(p_config, argc, argv);
  if (rval != 0) {
    app_print_usage(argc, argv);
    goto EXIT;
  }

  // 1. app init
  rval = app_init(p_app);
  if (rval < 0) {
    LOGE("%s init failed, rval=%d", TAG_APP, rval);
    goto EXIT;
  }

  // 2. API: init agora rtc sdk
  int32_t appid_len = strlen(p_config->p_appid);
  void *p_appid = (void *)(appid_len == 0 ? NULL : p_config->p_appid);
  rtc_service_option_t service_opt = { 0 };
  service_opt.area_code = AREA_CODE_GLOB;
  service_opt.log_cfg.log_path = p_config->p_sdk_log_dir;
  service_opt.log_cfg.log_level = RTC_LOG_INFO;
	snprintf(service_opt.license_value, sizeof(service_opt.license_value), "%s", p_config->p_license);
  rval = agora_rtc_init(p_appid, &event_handler, &service_opt);
  if (rval < 0) {
    LOGE("%s agora sdk init failed, rval=%d error=%s", TAG_API, rval, agora_rtc_err_2_str(rval));
    goto EXIT;
  }

  // 3. API:
  rval = agora_rtc_login_rtm(p_config->p_rtm_uid, p_config->p_token, &rtm_handler);
  if (rval < 0) {
    LOGE("login rtm failed");
    goto EXIT;
  }

  // 4. wait until rtm login success or Ctrl-C trigger stop
  while (1) {
    if (p_app->b_stop_flag || p_app->b_login_success_flag) {
      break;
    }
    util_sleep_ms(10);
  }

  // 5. rtm transmit loop
  while (!p_app->b_stop_flag) {
    if (p_config->send_enable_flag) {
      send_file_proc(p_app);
    } else {
      util_sleep_ms(1000);
    }
  }

  // 6. API: logout rtm
  agora_rtc_logout_rtm();

  // 7. API: fini rtc sdk
  agora_rtc_fini();

EXIT:
  // 8. app deinit
  app_deinit(p_app);
  return rval;
}
