#include "key_service/mybot_key_service.h"

#include "api/aosl_log.h"
#include <hal/aosl_hal_socket.h>

#include <stdlib.h>

typedef struct {
    mybot_key_event_handler_t emit;
    void *user_data;
} mybot_key_stdin_ctx_t;

static int key_stdin_init(void **out_ctx, mybot_key_event_handler_t emit, void *user_data) {
    if (!out_ctx || !emit) {
        return -1;
    }

    mybot_key_stdin_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }
    if (aosl_hal_sk_set_nonblock((aosl_fd_t)0) < 0) {
        free(ctx);
        return -1;
    }

    ctx->emit = emit;
    ctx->user_data = user_data;
    *out_ctx = ctx;
    return 0;
}

static int key_stdin_poll(void *opaque) {
    mybot_key_stdin_ctx_t *ctx = opaque;
    char ch;
    if (aosl_hal_sk_read((aosl_fd_t)0, &ch, 1) != 1) {
        return 0;
    }

    switch (ch) {
    case 's':
        ctx->emit(MYBOT_KEY_EVENT_CONVERSATION_START, ctx->user_data);
        break;
    case 'q':
        ctx->emit(MYBOT_KEY_EVENT_CONVERSATION_STOP, ctx->user_data);
        break;
    case 'p':
        ctx->emit(MYBOT_KEY_EVENT_PAIR, ctx->user_data);
        break;
    case 'e':
        ctx->emit(MYBOT_KEY_EVENT_EXIT, ctx->user_data);
        break;
    case '\n':
    case '\r':
        break;
    default:
        AOSL_LOG_INF("[KEY] '%c' ignored (s=start, q=stop, p=pair, e=exit)", ch);
        break;
    }
    return 0;
}

static void key_stdin_destroy(void *ctx) {
    free(ctx);
}

static const mybot_key_service_ops_t s_key_stdin_ops = {
    .name = "stdin",
    .init = key_stdin_init,
    .poll = key_stdin_poll,
    .destroy = key_stdin_destroy,
};

void mybot_key_platform_register_stdin(void) {
    if (mybot_key_service_register(&s_key_stdin_ops) < 0) {
        AOSL_LOG_ERR("key platform registration failed");
    }
}
