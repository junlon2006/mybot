/* SPDX-License-Identifier: Apache-2.0 */
#include <mybot/platform/mybot_wake_words.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    mybot_wake_words_handler_t handler;
    void *user_data;
} fake_wake_words_ctx_t;

static fake_wake_words_ctx_t s_fake;
static bool s_init_fails;
static bool s_emit_detection;
static int s_process_result;
static int s_init_count;
static int s_process_count;
static int s_destroy_count;
static const void *s_last_pcm;
static int s_last_frames;
static int s_handler_count;
static char s_last_wake_word[32];

static int fake_init(void **ctx, int sample_rate, int channels, int bits_per_sample,
                     mybot_wake_words_handler_t handler, void *user_data) {
    assert(sample_rate == 16000);
    assert(channels == 1);
    assert(bits_per_sample == 16);
    s_init_count++;
    if (s_init_fails) {
        return -1;
    }
    s_fake.handler = handler;
    s_fake.user_data = user_data;
    *ctx = &s_fake;
    return 0;
}

static int fake_process(void *ctx, const void *pcm, int frames) {
    assert(ctx == &s_fake);
    s_process_count++;
    s_last_pcm = pcm;
    s_last_frames = frames;
    if (s_emit_detection) {
        s_fake.handler("hello mybot", s_fake.user_data);
    }
    return s_process_result;
}

static void fake_destroy(void *ctx) {
    assert(ctx == &s_fake);
    s_fake.handler = NULL;
    s_fake.user_data = NULL;
    s_destroy_count++;
}

static void on_wake_word(const char *wake_word, void *user_data) {
    assert(user_data == &s_handler_count);
    assert(wake_word != NULL);
    size_t len = strlen(wake_word);
    assert(len < sizeof(s_last_wake_word));
    memcpy(s_last_wake_word, wake_word, len + 1);
    s_handler_count++;
}

int main(void) {
    const mybot_wake_words_ops_t incomplete_ops = {0};
    const mybot_wake_words_ops_t fake_ops = {
        .name = "fake",
        .init = fake_init,
        .process = fake_process,
        .destroy = fake_destroy,
    };
    int16_t pcm[960] = {0};

    assert(!mybot_wake_words_is_registered());
    assert(mybot_wake_words_register(NULL) < 0);
    assert(mybot_wake_words_register(&incomplete_ops) < 0);
    assert(mybot_wake_words_process(pcm, 960) < 0);
    assert(mybot_wake_words_init(16000, 1, 16, on_wake_word, &s_handler_count) < 0);

    assert(mybot_wake_words_register(&fake_ops) == 0);
    assert(mybot_wake_words_is_registered());
    assert(mybot_wake_words_init(0, 1, 16, on_wake_word, &s_handler_count) < 0);
    assert(mybot_wake_words_init(16000, 0, 16, on_wake_word, &s_handler_count) < 0);
    assert(mybot_wake_words_init(16000, 1, 0, on_wake_word, &s_handler_count) < 0);
    assert(mybot_wake_words_init(16000, 1, 16, NULL, NULL) < 0);

    s_init_fails = true;
    assert(mybot_wake_words_init(16000, 1, 16, on_wake_word, &s_handler_count) < 0);
    s_init_fails = false;
    assert(mybot_wake_words_init(16000, 1, 16, on_wake_word, &s_handler_count) == 0);
    assert(s_init_count == 2);
    assert(mybot_wake_words_init(16000, 1, 16, on_wake_word, &s_handler_count) < 0);
    assert(mybot_wake_words_register(&fake_ops) < 0);

    assert(mybot_wake_words_process(NULL, 960) < 0);
    assert(mybot_wake_words_process(pcm, 0) < 0);
    assert(mybot_wake_words_process(pcm, -1) < 0);

    s_emit_detection = true;
    assert(mybot_wake_words_process(pcm, 960) == 0);
    assert(s_process_count == 1);
    assert(s_last_pcm == pcm);
    assert(s_last_frames == 960);
    assert(s_handler_count == 1);
    assert(strcmp(s_last_wake_word, "hello mybot") == 0);

    s_emit_detection = false;
    s_process_result = -7;
    assert(mybot_wake_words_process(pcm, 320) == -7);
    assert(s_process_count == 2);
    assert(s_last_frames == 320);

    mybot_wake_words_deinit();
    mybot_wake_words_deinit();
    assert(s_destroy_count == 1);
    assert(s_fake.handler == NULL);
    assert(mybot_wake_words_process(pcm, 960) < 0);
    return 0;
}
