/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_announce_internal.h"
#include "platform_test.h"

#include "api/aosl.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mock implementation: prompt = 100 frames of 1000; digit d = 20 frames of 1000+d. */
#define MOCK_PROMPT_FRAMES 100
#define MOCK_DIGIT_FRAMES 20

static mybot_announce_t s_announce;

static int16_t s_prompt[MOCK_PROMPT_FRAMES];
static int16_t s_digits[10][MOCK_DIGIT_FRAMES];
static bool s_digit_available[10];
static bool s_prompt_available;
static bool s_invalid_read_count;
static int s_init_result;
static int s_init_count;
static int s_open_count;
static int s_close_count;
static int s_destroy_count;
static mybot_announce_sound_t s_error_sound = MYBOT_ANNOUNCE_SOUND_COUNT;

typedef struct {
    mybot_announce_sound_t sound;
    int offset;
} mock_handle_t;

static int mock_init(void **ctx) {
    *ctx = NULL;
    s_init_count++;
    return s_init_result;
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
    s_open_count++;
    return h;
}

static int mock_read(void *ctx, void *sound, int16_t *dst, int max_frames) {
    (void)ctx;
    if (s_invalid_read_count) {
        return max_frames + 1;
    }
    mock_handle_t *h = (mock_handle_t *)sound;
    if (!h) {
        return 0;
    }
    if (h->sound == s_error_sound) {
        return -1;
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
    assert(sound != NULL);
    s_close_count++;
    free(sound);
}

static void mock_destroy(void *ctx) {
    (void)ctx;
    assert(s_open_count == s_close_count);
    s_destroy_count++;
}

static const mybot_announce_ops_t s_mock_ops = {
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
        int n = mybot_announce_read_pcm(&s_announce, chunk, want, NULL);
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
    assert(mybot_announce_init(NULL) == -1);
    assert(mybot_announce_init(&s_announce) == 0);
    assert(!s_announce.active);
    assert(mybot_announce_play_pair_code(&s_announce, "42") == -1);
    assert(!mybot_announce_is_active(&s_announce));
    int16_t tmp[8];
    assert(mybot_announce_read_pcm(&s_announce, tmp, 8, NULL) == 0);
    uint32_t generation = 1;
    assert(mybot_announce_get_generation(&s_announce) == 0);
    assert(mybot_announce_read_pcm(&s_announce, tmp, 8, &generation) == 0);
    assert(generation == 0);
    assert(mybot_announce_play_pair_code(NULL, "42") == -1);
    assert(!mybot_announce_is_active(NULL));
    assert(mybot_announce_get_generation(NULL) == 0);
    mybot_announce_stop(NULL);
    mybot_announce_stop(&s_announce);
    mybot_announce_deinit(NULL);
    mybot_announce_deinit(&s_announce);
}

static void test_init_failure(void) {
    s_init_result = -1;
    assert(mybot_announce_init(&s_announce) == -1);
    assert(!s_announce.active);
    assert(s_announce.lock == NULL);
    assert(s_announce.ops_ctx == NULL);
    assert(mybot_announce_play_pair_code(&s_announce, "42") == -1);
    int16_t buf[8];
    uint32_t generation = 1;
    assert(mybot_announce_read_pcm(&s_announce, buf, 8, &generation) == 0);
    assert(generation == 0);
    mybot_announce_deinit(&s_announce);
    assert(s_destroy_count == 0);
    s_init_result = 0;
}

static void test_prompt_then_digits(void) {
    assert(mybot_announce_init(&s_announce) == 0);
    int init_count = s_init_count;
    assert(mybot_announce_init(&s_announce) == 0);
    assert(s_init_count == init_count);

    assert(mybot_announce_play_pair_code(&s_announce, "42") == 0);
    int16_t buf[256];
    int total = read_all(buf, 256);
    assert(total == MOCK_PROMPT_FRAMES + 2 * MOCK_DIGIT_FRAMES);
    expect_value(buf, 0, MOCK_PROMPT_FRAMES, 1000);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1004);
    expect_value(buf, MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES, MOCK_DIGIT_FRAMES, 1002);
    assert(!mybot_announce_is_active(&s_announce));
    assert(mybot_announce_read_pcm(&s_announce, buf, 1, NULL) == 0);
}

static void test_non_digit_and_empty_code(void) {
    /* Non-digits are skipped; only the prompt plays for an empty code. */
    assert(mybot_announce_play_pair_code(&s_announce, "4a2") == 0);
    int16_t buf[256];
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES + 2 * MOCK_DIGIT_FRAMES);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1004);
    expect_value(buf, MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES, MOCK_DIGIT_FRAMES, 1002);

    assert(mybot_announce_play_pair_code(&s_announce, "") == 0);
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES);
    assert(mybot_announce_play_pair_code(&s_announce, NULL) == 0);
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES);
}

static void test_stop_midway(void) {
    assert(mybot_announce_play_pair_code(&s_announce, "42") == 0);
    int16_t buf[64];
    assert(mybot_announce_read_pcm(&s_announce, buf, 50, NULL) == 50);
    assert(mybot_announce_is_active(&s_announce));
    mybot_announce_stop(&s_announce);
    assert(!mybot_announce_is_active(&s_announce));
    assert(mybot_announce_read_pcm(&s_announce, buf, 8, NULL) == 0);

    /* The implementation stays usable after a stop. */
    assert(mybot_announce_play_pair_code(&s_announce, "1") == 0);
    int16_t buf2[256];
    assert(read_all(buf2, 256) == MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES);
    expect_value(buf2, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1001);
}

static void test_replay_replaces(void) {
    assert(mybot_announce_play_pair_code(&s_announce, "42") == 0);
    int16_t tmp[16];
    assert(mybot_announce_read_pcm(&s_announce, tmp, 10, NULL) == 10);
    assert(mybot_announce_play_pair_code(&s_announce, "7") == 0);

    int16_t buf[256];
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES);
    expect_value(buf, 0, MOCK_PROMPT_FRAMES, 1000);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1007);
}

static void test_missing_sounds(void) {
    s_prompt_available = false;
    assert(mybot_announce_play_pair_code(&s_announce, "42") == -1);
    assert(!mybot_announce_is_active(&s_announce));
    s_prompt_available = true;

    /* A missing digit is skipped; the remaining digits still play. */
    s_digit_available[2] = false;
    assert(mybot_announce_play_pair_code(&s_announce, "42") == 0);
    int16_t buf[256];
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1004);
    s_digit_available[2] = true;
}

static void test_long_code_truncated(void) {
    /* 17 digits exceed the 16-digit queue cap; trailing digits are dropped
     * with a warning and the first 16 still play. */
    assert(mybot_announce_play_pair_code(&s_announce, "01234567890123456") == 0);
    int16_t buf[512];
    assert(read_all(buf, 512) == MOCK_PROMPT_FRAMES + 16 * MOCK_DIGIT_FRAMES);
    expect_value(buf, MOCK_PROMPT_FRAMES + 15 * MOCK_DIGIT_FRAMES, MOCK_DIGIT_FRAMES, 1005);
}

static void test_generation_and_frame_boundaries(void) {
    uint32_t previous = mybot_announce_get_generation(&s_announce);
    assert(mybot_announce_play_pair_code(&s_announce, "42") == 0);
    uint32_t current = mybot_announce_get_generation(&s_announce);
    assert(current == previous + 1);

    int16_t buf[256];
    uint32_t read_generation = 0;
    assert(mybot_announce_read_pcm(&s_announce, buf, 256, &read_generation) ==
           MOCK_PROMPT_FRAMES + 2 * MOCK_DIGIT_FRAMES);
    assert(read_generation == current);
    expect_value(buf, 0, MOCK_PROMPT_FRAMES, 1000);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1004);
    expect_value(buf, MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES, MOCK_DIGIT_FRAMES, 1002);
    assert(mybot_announce_get_generation(&s_announce) == current);

    assert(mybot_announce_play_pair_code(&s_announce, "7") == 0);
    assert(mybot_announce_get_generation(&s_announce) == current + 1);
    mybot_announce_stop(&s_announce);
    assert(mybot_announce_get_generation(&s_announce) == current + 2);
    assert(mybot_announce_read_pcm(&s_announce, buf, 256, &read_generation) == 0);
    assert(read_generation == current + 2);
}

static void test_failed_replacement_preserves_source(void) {
    assert(mybot_announce_play_pair_code(&s_announce, "7") == 0);
    int16_t buf[256];
    uint32_t generation = 0;
    assert(mybot_announce_read_pcm(&s_announce, buf, 10, &generation) == 10);
    s_prompt_available = false;
    assert(mybot_announce_play_pair_code(&s_announce, "42") == -1);
    s_prompt_available = true;
    assert(mybot_announce_get_generation(&s_announce) == generation);
    assert(mybot_announce_is_active(&s_announce));

    uint32_t read_generation = 0;
    assert(mybot_announce_read_pcm(&s_announce, buf, 256, &read_generation) ==
           MOCK_PROMPT_FRAMES - 10 + MOCK_DIGIT_FRAMES);
    assert(read_generation == generation);
    expect_value(buf, 0, MOCK_PROMPT_FRAMES - 10, 1000);
    expect_value(buf, MOCK_PROMPT_FRAMES - 10, MOCK_DIGIT_FRAMES, 1007);
}

static void test_invalid_read_count(void) {
    assert(mybot_announce_play_pair_code(&s_announce, "7") == 0);
    int16_t buf[16];
    s_invalid_read_count = true;
    assert(mybot_announce_read_pcm(&s_announce, buf, 16, NULL) == 0);
    s_invalid_read_count = false;
    assert(mybot_announce_read_pcm(&s_announce, buf, 16, NULL) == 16);
    expect_value(buf, 0, 16, 1000);
    mybot_announce_stop(&s_announce);
}

static void test_read_error_skips_sound(void) {
    assert(mybot_announce_play_pair_code(&s_announce, "42") == 0);
    s_error_sound = MYBOT_ANNOUNCE_SOUND_DIGIT_4;
    int16_t buf[256];
    assert(read_all(buf, 256) == MOCK_PROMPT_FRAMES + MOCK_DIGIT_FRAMES);
    expect_value(buf, 0, MOCK_PROMPT_FRAMES, 1000);
    expect_value(buf, MOCK_PROMPT_FRAMES, MOCK_DIGIT_FRAMES, 1002);
    assert(!mybot_announce_is_active(&s_announce));
    s_error_sound = MYBOT_ANNOUNCE_SOUND_COUNT;
}

static void test_invalid_read_inputs(void) {
    assert(mybot_announce_play_pair_code(&s_announce, "7") == 0);
    uint32_t current = mybot_announce_get_generation(&s_announce);
    uint32_t generation = current;
    int16_t buf[16];
    assert(mybot_announce_read_pcm(NULL, buf, 16, &generation) == 0);
    assert(generation == 0);
    generation = current;
    assert(mybot_announce_read_pcm(&s_announce, NULL, 16, &generation) == 0);
    assert(generation == 0);
    generation = current;
    assert(mybot_announce_read_pcm(&s_announce, buf, 0, &generation) == 0);
    assert(generation == 0);
    assert(mybot_announce_read_pcm(&s_announce, buf, -1, &generation) == 0);
    assert(mybot_announce_is_active(&s_announce));
    assert(mybot_announce_get_generation(&s_announce) == current);
    assert(mybot_announce_read_pcm(&s_announce, buf, 16, &generation) == 16);
    expect_value(buf, 0, 16, 1000);
    mybot_announce_stop(&s_announce);
}

static void test_deinit_closes_pending_sounds(void) {
    assert(mybot_announce_play_pair_code(&s_announce, "42") == 0);
    int16_t buf[MOCK_PROMPT_FRAMES + 5];
    assert(mybot_announce_read_pcm(&s_announce, buf, MOCK_PROMPT_FRAMES + 5, NULL) ==
           MOCK_PROMPT_FRAMES + 5);
    assert(mybot_announce_is_active(&s_announce));
    int closes = s_close_count;
    mybot_announce_deinit(&s_announce);
    assert(s_close_count == closes + 2);
    assert(s_destroy_count == 1);
    assert(!s_announce.active);
    assert(s_announce.lock == NULL);
    assert(s_announce.ops_ctx == NULL);
    assert(s_announce.queue_len == 0);
    assert(s_announce.queue_pos == 0);
    mybot_announce_deinit(&s_announce);
    assert(s_destroy_count == 1);

    assert(mybot_announce_init(&s_announce) == 0);
    assert(mybot_announce_play_pair_code(&s_announce, "7") == 0);
    mybot_announce_deinit(&s_announce);
    assert(s_destroy_count == 2);
    assert(s_open_count == s_close_count);
}

int main(void) {
    setup_mock_sounds();
    aosl_ctor();

    test_not_registered();
    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.announce = &s_mock_ops;
    assert(mybot_platform_register(&descriptor) == 0);
    test_init_failure();
    test_prompt_then_digits();
    test_non_digit_and_empty_code();
    test_stop_midway();
    test_replay_replaces();
    test_missing_sounds();
    test_long_code_truncated();
    test_generation_and_frame_boundaries();
    test_failed_replacement_preserves_source();
    test_invalid_read_count();
    test_read_error_skips_sound();
    test_invalid_read_inputs();
    test_deinit_closes_pending_sounds();

    mybot_announce_deinit(&s_announce);
    aosl_dtor();
    printf("announce_test: all tests passed\n");
    return 0;
}
