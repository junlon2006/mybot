/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_announce_internal.h"

#include "api/aosl.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mock implementation: prompt = 100 frames of 1000; digit d = 20 frames of 1000+d. */
#define MOCK_PROMPT_FRAMES 100
#define MOCK_DIGIT_FRAMES 20

static int16_t s_prompt[MOCK_PROMPT_FRAMES];
static int16_t s_digits[10][MOCK_DIGIT_FRAMES];
static bool s_digit_available[10];
static bool s_prompt_available;

typedef struct {
    mybot_announce_sound_t sound;
    int offset;
} mock_handle_t;

static int mock_init(void **ctx) {
    *ctx = NULL;
    return 0;
}

static void *mock_open(void *ctx, mybot_announce_sound_t sound) {
    (void)ctx;
    if (sound == MYBOT_ANNOUNCE_SOUND_PROMPT) {
        if (!s_prompt_available) {
            return NULL;
        }
    } else if (sound >= MYBOT_ANNOUNCE_SOUND_DIGIT_0 && sound <= MYBOT_ANNOUNCE_SOUND_DIGIT_9) {
        if (!s_digit_available[sound - MYBOT_ANNOUNCE_SOUND_DIGIT_0]) {
            return NULL;
        }
    } else {
        return NULL;
    }
    mock_handle_t *h = (mock_handle_t *)malloc(sizeof(*h));
    if (!h) {
        return NULL;
    }
    h->sound = sound;
    h->offset = 0;
    return h;
}

static int mock_read(void *ctx, void *sound, int16_t *dst, int max_frames) {
    (void)ctx;
    mock_handle_t *h = (mock_handle_t *)sound;
    if (!h) {
        return 0;
    }
    const int16_t *src;
    int frames;
    if (h->sound == MYBOT_ANNOUNCE_SOUND_PROMPT) {
        src = s_prompt;
        frames = MOCK_PROMPT_FRAMES;
    } else {
        src = s_digits[h->sound - MYBOT_ANNOUNCE_SOUND_DIGIT_0];
        frames = MOCK_DIGIT_FRAMES;
    }
    int remaining = frames - h->offset;
    int n = remaining < max_frames ? remaining : max_frames;
    if (n <= 0) {
        return 0;
    }
    memcpy(dst, src + h->offset, (size_t)n * sizeof(int16_t));
    h->offset += n;
    return n;
}

static void mock_close(void *ctx, void *sound) {
    (void)ctx;
    free(sound);
}

static void mock_destroy(void *ctx) {
    (void)ctx;
}

static const mybot_announce_ops_t s_mock_ops = {
    .name = "mock",
    .init = mock_init,
    .open = mock_open,
    .read = mock_read,
    .close = mock_close,
    .destroy = mock_destroy,
};

static void setup_mock_sounds(void) {
    for (int i = 0; i < MOCK_PROMPT_FRAMES; i++) {
        s_prompt[i] = 1000;
    }
    for (int d = 0; d < 10; d++) {
        s_digit_available[d] = true;
        for (int i = 0; i < MOCK_DIGIT_FRAMES; i++) {
            s_digits[d][i] = (int16_t)(1000 + d);
        }
    }
    s_prompt_available = true;
}

static int read_all(int16_t *out, int max_frames) {
    int total = 0;
    int16_t chunk[64];
    while (total < max_frames) {
        int want = max_frames - total < 64 ? max_frames - total : 64;
        int n = mybot_announce_read_pcm(chunk, want);
        if (n == 0) {
            break;
        }
        memcpy(out + total, chunk, (size_t)n * sizeof(int16_t));
        total += n;
    }
    return total;
}

static void expect_value(const int16_t *buf, int start, int count, int16_t value) {
    for (int i = 0; i < count; i++) {
        assert(buf[start + i] == value);
    }
}

static void test_not_registered(void) {
    assert(!mybot_announce_is_registered());
    assert(mybot_announce_play_pair_code("42") == -1);
    assert(!mybot_announce_is_active());
    int16_t tmp[8];
    assert(mybot_announce_read_pcm(tmp, 8) == 0);
}

static void test_prompt_then_digits(void) {
    assert(mybot_announce_register(&s_mock_ops) == 0);
    assert(mybot_announce_init() == 0);

    assert(mybot_announce_play_pair_code("42") == 0);
    int16_t buf[256];
    int total = read_all(buf, 256);
    assert(total == MOCK_PROMPT_FRAMES + 2 * MOCK_DIGIT_FRAMES);
    expect_value(buf, 0, MOCK_PROMPT_FRAMES, 1000);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1004);
    expect_value(buf, MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES, MOCK_DIGIT_FRAMES, 1002);
    assert(!mybot_announce_is_active());
    assert(mybot_announce_read_pcm(buf, 1) == 0);
}

static void test_non_digit_and_empty_code(void) {
    /* Non-digits are skipped; only the prompt plays for an empty code. */
    assert(mybot_announce_play_pair_code("4a2") == 0);
    int16_t buf[256];
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES + 2 * MOCK_DIGIT_FRAMES);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1004);
    expect_value(buf, MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES, MOCK_DIGIT_FRAMES, 1002);

    assert(mybot_announce_play_pair_code("") == 0);
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES);
}

static void test_stop_midway(void) {
    assert(mybot_announce_play_pair_code("42") == 0);
    int16_t buf[64];
    assert(mybot_announce_read_pcm(buf, 50) == 50);
    assert(mybot_announce_is_active());
    mybot_announce_stop();
    assert(!mybot_announce_is_active());
    assert(mybot_announce_read_pcm(buf, 8) == 0);

    /* The implementation stays usable after a stop. */
    assert(mybot_announce_play_pair_code("1") == 0);
    int16_t buf2[256];
    assert(read_all(buf2, 256) == MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES);
    expect_value(buf2, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1001);
}

static void test_replay_replaces(void) {
    assert(mybot_announce_play_pair_code("42") == 0);
    int16_t tmp[16];
    assert(mybot_announce_read_pcm(tmp, 10) == 10);
    assert(mybot_announce_play_pair_code("7") == 0);

    int16_t buf[256];
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES);
    expect_value(buf, 0, MOCK_PROMPT_FRAMES, 1000);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1007);
}

static void test_missing_sounds(void) {
    s_prompt_available = false;
    assert(mybot_announce_play_pair_code("42") == -1);
    assert(!mybot_announce_is_active());
    s_prompt_available = true;

    /* A missing digit is skipped; the remaining digits still play. */
    s_digit_available[2] = false;
    assert(mybot_announce_play_pair_code("42") == 0);
    int16_t buf[256];
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1004);
    s_digit_available[2] = true;
}

static void test_long_code_truncated(void) {
    /* 17 digits exceed the 16-digit queue cap; trailing digits are dropped
     * with a warning and the first 16 still play. */
    assert(mybot_announce_play_pair_code("01234567890123456") == 0);
    int16_t buf[512];
    assert(read_all(buf, 512) == MOCK_PROMPT_FRAMES + 16 * MOCK_DIGIT_FRAMES);
    expect_value(buf, MOCK_PROMPT_FRAMES + 15 * MOCK_DIGIT_FRAMES, MOCK_DIGIT_FRAMES, 1005);
}

int main(void) {
    setup_mock_sounds();
    aosl_ctor();

    test_not_registered();
    test_prompt_then_digits();
    test_non_digit_and_empty_code();
    test_stop_midway();
    test_replay_replaces();
    test_missing_sounds();
    test_long_code_truncated();

    mybot_announce_deinit();
    aosl_dtor();
    printf("announce_test: all tests passed\n");
    return 0;
}
