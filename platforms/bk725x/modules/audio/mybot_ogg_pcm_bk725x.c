/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_ogg_pcm_bk725x.h"

#include "bk_posix.h"
#include "sdkconfig.h"

#include <common/bk_err.h>
#include "mybot_platform_log.h"
#include <os/mem.h>

#include <modules/ogg.h>
#include <modules/opus.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TAG "mybot_ogg"

#define MYBOT_OGG_MAX_FILE_BYTES (256U * 1024U)
#define MYBOT_OGG_FEED_CHUNK 2048
#define MYBOT_OGG_EST_MARGIN_RATE_DIV 20 /* ~50 ms tail safety margin */

#define OPUS_HEAD_MAGIC "OpusHead"
#define OPUS_TAGS_MAGIC "OpusTags"
#define OPUS_HEAD_CHANNELS_OFF 9
#define OPUS_HEAD_PRESKIP_OFF 10
#define OPUS_HEAD_MIN_LEN 19

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int parse_opus_head(const unsigned char *pkt, int bytes, uint8_t *channels,
                           uint16_t *pre_skip) {
    if (!pkt || bytes < OPUS_HEAD_MIN_LEN || memcmp(pkt, OPUS_HEAD_MAGIC, 8) != 0) {
        return -1;
    }
    if (pkt[8] != 1) { /* only OpusHead v1 is supported */
        return -1;
    }
    *channels = pkt[OPUS_HEAD_CHANNELS_OFF];
    *pre_skip = read_le16(pkt + OPUS_HEAD_PRESKIP_OFF);
    return 0;
}

static int is_stream_packet(const unsigned char *pkt, int bytes) {
    /* OpusHead and OpusTags are stream headers, not audio packets. */
    return !(bytes >= 8 && (memcmp(pkt, OPUS_HEAD_MAGIC, 8) == 0 ||
                            memcmp(pkt, OPUS_TAGS_MAGIC, 8) == 0));
}

/* Feed the next chunk of the in-memory file into the libogg sync layer. */
static int ogg_feed(ogg_sync_state *oy, const uint8_t *file_data, size_t file_len,
                    size_t *consumed) {
    size_t take = file_len - *consumed;
    char *sync_buf;

    if (take > MYBOT_OGG_FEED_CHUNK) {
        take = MYBOT_OGG_FEED_CHUNK;
    }
    sync_buf = ogg_sync_buffer(oy, (long)take);
    if (!sync_buf) {
        MYBOT_LOGW(TAG, "ogg sync buffer allocation failed");
        return -1;
    }
    memcpy(sync_buf, file_data + *consumed, take);
    if (ogg_sync_wrote(oy, (long)take) < 0) {
        MYBOT_LOGW(TAG, "ogg_sync_wrote failed");
        return -1;
    }
    *consumed += take;
    return 0;
}

/* Append cnt mono samples taken from a possibly-stereo decode buffer. */
static size_t append_mono(int16_t *pcm, const opus_int16 *src, int channels, int offset,
                          int cnt, size_t dst_offset) {
    if (channels == 1) {
        memcpy(pcm + dst_offset, src + offset, (size_t)cnt * sizeof(int16_t));
    } else {
        for (int i = 0; i < cnt; i++) {
            pcm[dst_offset + (size_t)i] =
                (int16_t)(((int)src[(offset + i) * channels] +
                           (int)src[(offset + i) * channels + 1]) >>
                          1);
        }
    }
    return dst_offset + (size_t)cnt;
}

int mybot_ogg_pcm_load_memory(const char *path, const uint8_t *file_data, size_t file_len,
                              int out_rate, mybot_ogg_pcm_t *out) {
    ogg_sync_state oy;
    ogg_stream_state os;
    ogg_page og;
    ogg_packet op;
    OpusDecoder *dec = NULL;
    opus_int16 *tmp = NULL;
    int16_t *pcm = NULL;
    bool stream_inited = false;
    bool head_parsed = false;
    bool truncated = false;
    uint8_t channels = 1;
    uint16_t pre_skip = 0;
    ogg_int64_t last_granule = 0;
    size_t consumed;
    size_t capacity = 0;
    size_t written = 0;
    int skip = 0;
    int tmp_cap;
    int result = -1;

    if (!path || !file_data || file_len == 0 || file_len > MYBOT_OGG_MAX_FILE_BYTES ||
        !out || (out_rate != 8000 && out_rate != 12000 && out_rate != 16000 &&
                          out_rate != 24000 && out_rate != 48000)) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    memset(&oy, 0, sizeof(oy));
    memset(&os, 0, sizeof(os));
    memset(&og, 0, sizeof(og));
    memset(&op, 0, sizeof(op));

    if (file_len == 0 || file_len > MYBOT_OGG_MAX_FILE_BYTES) {
        MYBOT_LOGW(TAG, "invalid or missing ogg: %s", path);
        return -1;
    }

    /* ---- Pass 1: walk pages for the stream granule position and OpusHead. */
    if (ogg_sync_init(&oy) != 0) {
        MYBOT_LOGE(TAG, "ogg_sync_init failed");
        goto cleanup;
    }
    consumed = 0;
    while (consumed < file_len) {
        int r;

        if (ogg_feed(&oy, file_data, file_len, &consumed) < 0) {
            goto cleanup;
        }
        while ((r = ogg_sync_pageout(&oy, &og)) == 1) {
            if (ogg_page_granulepos(&og) >= 0) {
                last_granule = ogg_page_granulepos(&og);
            }
            if (!head_parsed) {
                if (!stream_inited) {
                    if (ogg_stream_init(&os, ogg_page_serialno(&og)) != 0) {
                        MYBOT_LOGE(TAG, "ogg_stream_init failed");
                        goto cleanup;
                    }
                    stream_inited = true;
                }
                if (ogg_stream_pagein(&os, &og) < 0) {
                    continue;
                }
                while (ogg_stream_packetout(&os, &op) == 1) {
                    if (parse_opus_head(op.packet, op.bytes, &channels, &pre_skip) < 0) {
                        MYBOT_LOGW(TAG, "not an opus stream: %s", path);
                        goto cleanup;
                    }
                    head_parsed = true;
                    break;
                }
            }
        }
        if (r == -1) {
            MYBOT_LOGW(TAG, "ogg sync error");
            goto cleanup;
        }
    }
    ogg_stream_clear(&os);
    stream_inited = false;
    ogg_sync_clear(&oy);

    if (!head_parsed) {
        MYBOT_LOGW(TAG, "no opus head found in %s", path);
        goto cleanup;
    }

    /* ---- Size the PCM buffer from the stream granule position (48 kHz). */
    if (last_granule > (ogg_int64_t)pre_skip) {
        capacity = (size_t)((last_granule - pre_skip) * out_rate / 48000);
    } else {
        capacity = (size_t)out_rate; /* fallback: ~1 s, grows via exact path below */
        MYBOT_LOGW(TAG, "no granule position, using fallback size");
    }
    capacity += (size_t)out_rate / MYBOT_OGG_EST_MARGIN_RATE_DIV; /* ~50 ms tail margin */
    pcm = psram_malloc(capacity * sizeof(int16_t));
    if (!pcm) {
        MYBOT_LOGE(TAG, "decoded pcm allocation failed: %u frames", (unsigned int)capacity);
        goto cleanup;
    }

    /* ---- Pass 2: full demux + decode into the pre-sized buffer. */
    if (ogg_sync_init(&oy) != 0) {
        MYBOT_LOGE(TAG, "ogg_sync_init failed");
        goto cleanup;
    }
    dec = opus_decoder_create((opus_int32)out_rate, (int)channels, &result);
    if (!dec) {
        MYBOT_LOGE(TAG, "opus_decoder_create failed: %d", result);
        goto cleanup;
    }
    tmp_cap = out_rate / 5 + 16; /* > max 120 ms frame */
    tmp = psram_malloc((size_t)tmp_cap * channels * sizeof(opus_int16));
    if (!tmp) {
        MYBOT_LOGE(TAG, "opus decode buffer allocation failed");
        goto cleanup;
    }
    skip = ((int)pre_skip * out_rate) / 48000;

    consumed = 0;
    while (consumed < file_len) {
        int r;

        if (ogg_feed(&oy, file_data, file_len, &consumed) < 0) {
            goto cleanup;
        }
        while ((r = ogg_sync_pageout(&oy, &og)) == 1 && !truncated) {
            if (!stream_inited) {
                if (ogg_stream_init(&os, ogg_page_serialno(&og)) != 0) {
                    MYBOT_LOGE(TAG, "ogg_stream_init failed");
                    goto cleanup;
                }
                stream_inited = true;
            }
            if (ogg_stream_pagein(&os, &og) < 0) {
                continue;
            }
            for (;;) {
                int p = ogg_stream_packetout(&os, &op);
                int n;
                int src = 0;
                int cnt;

                if (p == 0) {
                    break;
                }
                if (p < 0) {
                    continue;
                }
                if (!is_stream_packet(op.packet, op.bytes)) {
                    continue;
                }
                n = opus_decode(dec, op.packet, (opus_int32)op.bytes, tmp, tmp_cap, 0);
                if (n < 0) {
                    continue; /* drop one malformed packet */
                }
                if (skip > 0) {
                    if (n <= skip) {
                        skip -= n;
                        continue;
                    }
                    src = skip;
                    skip = 0;
                }
                cnt = n - src;
                if (written + (size_t)cnt > capacity) {
                    MYBOT_LOGW(TAG, "decoded pcm exceeds estimate, truncated");
                    truncated = true;
                    break;
                }
                written = append_mono(pcm, tmp, channels, src, cnt, written);
            }
        }
        if (r == -1) {
            MYBOT_LOGW(TAG, "ogg sync error");
            goto cleanup;
        }
        if (truncated) {
            break;
        }
    }

    if (written == 0) {
        MYBOT_LOGW(TAG, "no pcm decoded from %s", path);
        goto cleanup;
    }

    out->pcm = pcm;
    out->frames = (int)written;
    out->sample_rate = out_rate;
    pcm = NULL;
    result = 0;
    MYBOT_LOGI(TAG, "ogg decoded: %s, frames=%d rate=%d ch=%u", path, (int)written,
               out_rate, channels);

cleanup:
    if (dec) {
        opus_decoder_destroy(dec);
    }
    if (tmp) {
        psram_free(tmp);
    }
    if (pcm) {
        psram_free(pcm);
    }
    if (stream_inited) {
        ogg_stream_clear(&os);
    }
    ogg_sync_clear(&oy);
    return result;
}

void mybot_ogg_pcm_free(mybot_ogg_pcm_t *ogg) {
    if (!ogg || !ogg->pcm) {
        return;
    }
    psram_free(ogg->pcm);
    ogg->pcm = NULL;
    ogg->frames = 0;
}
