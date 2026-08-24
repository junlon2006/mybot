/* SPDX-License-Identifier: Apache-2.0 */
#include "linux_platform_adapters.h"
#include "platform_test.h"

#include "mybot_announce_internal.h"

#include "api/aosl.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROMPT_FRAMES 200
#define DIGIT_FRAMES 30

static char s_base_dir[512]; /* /tmp/mybot_announce_file_test_<pid> */
static mybot_announce_t s_announce;
static char s_assets_dir[640];  /* .../assets */
static char s_locales_dir[768]; /* .../assets/locales */
static char s_locale_dir[896];  /* .../assets/locales/zh-CN */

static void write_pcm(const char *path, const int16_t *pcm, int frames) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(pcm, 2, (size_t)frames, f) == (size_t)frames);
    fclose(f);
}

static int read_all(int16_t *out, int max_frames) {
    int total = 0;
    int16_t chunk[64];
    while (total < max_frames) {
        int want = max_frames - total < 64 ? max_frames - total : 64;
        int n = mybot_announce_read_pcm(&s_announce, chunk, want);
        if (n == 0) {
            break;
        }
        memcpy(out + total, chunk, (size_t)n * sizeof(int16_t));
        total += n;
    }
    return total;
}

int main(void) {
    snprintf(s_base_dir, sizeof(s_base_dir), "/tmp/mybot_announce_file_test_%d", getpid());
    snprintf(s_assets_dir, sizeof(s_assets_dir), "%s/assets", s_base_dir);
    snprintf(s_locales_dir, sizeof(s_locales_dir), "%s/locales", s_assets_dir);
    snprintf(s_locale_dir, sizeof(s_locale_dir), "%s/zh-CN", s_locales_dir);
    assert(mkdir(s_base_dir, 0755) == 0);
    assert(mkdir(s_assets_dir, 0755) == 0);
    assert(mkdir(s_locales_dir, 0755) == 0);
    assert(mkdir(s_locale_dir, 0755) == 0);

    int16_t prompt[PROMPT_FRAMES];
    int16_t digit5[DIGIT_FRAMES];
    char path[1024];
    for (int i = 0; i < PROMPT_FRAMES; i++) {
        prompt[i] = (int16_t)(i * 7);
    }
    for (int i = 0; i < DIGIT_FRAMES; i++) {
        digit5[i] = (int16_t)(5000 + i);
    }
    snprintf(path, sizeof(path), "%s/prompt.pcm", s_locale_dir);
    write_pcm(path, prompt, PROMPT_FRAMES);
    snprintf(path, sizeof(path), "%s/5.pcm", s_locale_dir);
    write_pcm(path, digit5, DIGIT_FRAMES);

    setenv("MYBOT_ASSETS_DIR", s_assets_dir, 1);
    setenv("MYBOT_LOCALE", "zh-CN", 1);

    aosl_ctor();
    mybot_platform_descriptor_t descriptor = mybot_test_platform_descriptor();
    descriptor.capabilities |= MYBOT_PLATFORM_CAP_ANNOUNCE;
    descriptor.announce = linux_announce_platform_file_ops();
    assert(mybot_platform_register(&descriptor) == 0);
    assert(mybot_announce_is_registered());
    assert(mybot_announce_init(&s_announce) == 0);

    /* Prompt then digit 5, in order. */
    assert(mybot_announce_play_pair_code(&s_announce, "5") == 0);
    int16_t buf[256];
    assert(read_all(buf, 256) == PROMPT_FRAMES + DIGIT_FRAMES);
    for (int i = 0; i < PROMPT_FRAMES; i++) {
        assert(buf[i] == prompt[i]);
    }
    for (int i = 0; i < DIGIT_FRAMES; i++) {
        assert(buf[PROMPT_FRAMES + i] == digit5[i]);
    }

    /* A missing digit file is skipped; the prompt still plays. */
    assert(mybot_announce_play_pair_code(&s_announce, "9") == 0);
    assert(read_all(buf, 256) == PROMPT_FRAMES);
    assert(buf[0] == prompt[0]);
    assert(buf[PROMPT_FRAMES - 1] == prompt[PROMPT_FRAMES - 1]);

    /* A missing prompt aborts the whole announcement. */
    snprintf(path, sizeof(path), "%s/prompt.pcm", s_locale_dir);
    assert(remove(path) == 0);
    assert(mybot_announce_play_pair_code(&s_announce, "5") == -1);
    assert(!mybot_announce_is_active(&s_announce));

    mybot_announce_deinit(&s_announce);
    aosl_dtor();

    unsetenv("MYBOT_ASSETS_DIR");
    unsetenv("MYBOT_LOCALE");
    snprintf(path, sizeof(path), "%s/5.pcm", s_locale_dir);
    remove(path);
    remove(s_locale_dir);
    remove(s_locales_dir);
    remove(s_assets_dir);
    remove(s_base_dir);

    printf("announce_file_test: all tests passed\n");
    return 0;
}
