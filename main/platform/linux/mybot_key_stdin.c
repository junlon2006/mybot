#include "key_service/mybot_key_service.h"

#include "api/aosl_log.h"
#include "api/aosl_mpq.h"
#include "api/aosl_mpq_fd.h"
#include "hal/aosl_hal_thread.h"

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#define KEY_STDIN_MPQ_STACK_SIZE 4096
#define KEY_STDIN_MPQ_MAX_EVENTS 16
#define KEY_STDIN_READ_BUFFER_SIZE 1024

typedef struct {
    mybot_key_event_handler_t emit;
    void *user_data;
    aosl_mpq_t mpq;
    bool stdin_registered;
} mybot_key_stdin_ctx_t;

static void key_stdin_detach(mybot_key_stdin_ctx_t *ctx) {
    if (!ctx->stdin_registered) {
        return;
    }

    ctx->stdin_registered = false;
    if (aosl_mpq_del_fd(STDIN_FILENO) < 0) {
        AOSL_LOG_WRN("[KEY] failed to detach stdin from event queue");
    }
}

static void key_stdin_emit_char(mybot_key_stdin_ctx_t *ctx, char ch) {
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
}

static isize_t key_stdin_check_packet(const void *data, size_t len, uintptr_t argc,
                                      uintptr_t argv[]) {
    (void)data;
    (void)argc;
    (void)argv;
    return (isize_t)len;
}

static void key_stdin_on_data(void *data, size_t len, uintptr_t argc, uintptr_t argv[]) {
    (void)argc;
    mybot_key_stdin_ctx_t *ctx = (mybot_key_stdin_ctx_t *)argv[0];
    if (len == 0) {
        return;
    }

    const char *chars = data;
    for (size_t i = 0; i < len; ++i) {
        key_stdin_emit_char(ctx, chars[i]);
    }
}

static void key_stdin_on_event(aosl_fd_t fd, int event, uintptr_t argc, uintptr_t argv[]) {
    (void)fd;
    (void)argc;
    mybot_key_stdin_ctx_t *ctx = (mybot_key_stdin_ctx_t *)argv[0];
    if (event == AOSL_IOFD_HUP || event < 0) {
        ctx->stdin_registered = false;
        AOSL_LOG_WRN("[KEY] stdin event source closed (event=%d)", event);
    }
}

static int key_stdin_mpq_init(void *arg) {
    mybot_key_stdin_ctx_t *ctx = arg;
    if (aosl_mpq_add_fd(STDIN_FILENO, KEY_STDIN_READ_BUFFER_SIZE, AOSL_DEFAULT_READ_FN, NULL,
                        key_stdin_check_packet, key_stdin_on_data, key_stdin_on_event, 1,
                        (uintptr_t)ctx) < 0) {
        AOSL_LOG_ERR("[KEY] failed to register stdin event source");
        return -1;
    }

    ctx->stdin_registered = true;
    return 0;
}

static void key_stdin_mpq_fini(void *arg) {
    key_stdin_detach(arg);
}

static int key_stdin_init(void **out_ctx, mybot_key_event_handler_t emit, void *user_data) {
    if (!out_ctx || !emit) {
        return -1;
    }

    mybot_key_stdin_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }

    ctx->emit = emit;
    ctx->user_data = user_data;
    ctx->mpq =
        aosl_mpq_create(AOSL_THRD_PRI_NORMAL, KEY_STDIN_MPQ_STACK_SIZE, KEY_STDIN_MPQ_MAX_EVENTS,
                        "key_stdin_mpq", key_stdin_mpq_init, key_stdin_mpq_fini, ctx);
    if (aosl_mpq_invalid(ctx->mpq)) {
        free(ctx);
        return -1;
    }

    *out_ctx = ctx;
    return 0;
}

static void key_stdin_destroy(void *opaque) {
    mybot_key_stdin_ctx_t *ctx = opaque;
    aosl_mpq_destroy_wait(ctx->mpq);
    free(ctx);
}

static const mybot_key_service_ops_t s_key_stdin_ops = {
    .name = "stdin",
    .init = key_stdin_init,
    .destroy = key_stdin_destroy,
};

void mybot_key_platform_register_stdin(void) {
    if (mybot_key_service_register(&s_key_stdin_ops) < 0) {
        AOSL_LOG_ERR("key platform registration failed");
    }
}
