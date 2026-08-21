/* SPDX-License-Identifier: Apache-2.0 */
#include "mybot_wifi_credentials.h"

#include <bk_ef.h>
#include "mybot_platform_log.h"
#include <components/netif_types.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WIFI_CREDENTIALS_TAG "mybot_wifi"
#define CREDENTIAL_LOGI(format, ...) \
    MYBOT_LOGI(WIFI_CREDENTIALS_TAG, format, ##__VA_ARGS__)
#define CREDENTIAL_LOGW(format, ...) \
    MYBOT_LOGW(WIFI_CREDENTIALS_TAG, format, ##__VA_ARGS__)

#define WIFI_CREDENTIAL_KEY "mybot_wifi_cred_v1"
#define WIFI_CREDENTIAL_MAGIC 0x4d425746u
#define WIFI_CREDENTIAL_VERSION 2u
#define WIFI_LEGACY_CREDENTIAL_VERSION 1u
#define WIFI_BK_LEGACY_CREDENTIAL_KEY "d_network_id"
#define WIFI_BK_LEGACY_CREDENTIAL_MAGIC 0x8000u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint8_t ssid_length;
    uint8_t password_length;
    uint8_t reserved[2];
    uint8_t ssid[WIFI_SSID_STR_LEN];
    uint8_t password[WIFI_PASSWORD_LEN];
    uint8_t reserved_tail[2];
} mybot_wifi_credentials_v1_t;

typedef struct {
    uint8_t sta_ssid[WIFI_SSID_STR_LEN];
    uint8_t sta_password[WIFI_PASSWORD_LEN];
    uint8_t ap_ssid[WIFI_SSID_STR_LEN];
    uint8_t ap_password[WIFI_PASSWORD_LEN];
    uint8_t ap_channel;
    uint8_t padding;
    uint16_t flag;
} mybot_wifi_bk_legacy_credentials_t;

_Static_assert(sizeof(mybot_wifi_credentials_v1_t) == 112, "wifi credentials v1 size");
_Static_assert(offsetof(mybot_wifi_bk_legacy_credentials_t, flag) == 198,
               "legacy credentials flag offset");
_Static_assert(sizeof(mybot_wifi_bk_legacy_credentials_t) == 200,
               "legacy credentials size");

static bool string_is_terminated(const uint8_t *value, size_t capacity) {
    return value && memchr(value, '\0', capacity) != NULL;
}

static bool credential_lengths_are_valid(size_t ssid_length, size_t password_length) {
    return ssid_length > 0 && ssid_length < WIFI_SSID_STR_LEN &&
           password_length < WIFI_PASSWORD_LEN;
}

bool mybot_wifi_credential_is_valid(const mybot_wifi_credential_entry_t *entry) {
    return entry &&
           credential_lengths_are_valid(entry->ssid_length, entry->password_length) &&
           !memchr(entry->ssid, '\0', entry->ssid_length) &&
           !memchr(entry->password, '\0', entry->password_length) &&
           entry->ssid[entry->ssid_length] == '\0' &&
           entry->password[entry->password_length] == '\0';
}

void mybot_wifi_credentials_init(mybot_wifi_credential_list_t *list) {
    if (!list) {
        return;
    }
    memset(list, 0, sizeof(*list));
    list->magic = WIFI_CREDENTIAL_MAGIC;
    list->version = WIFI_CREDENTIAL_VERSION;
    list->record_size = sizeof(*list);
}

int mybot_wifi_credentials_save(const mybot_wifi_credential_list_t *list) {
    if (!list || list->magic != WIFI_CREDENTIAL_MAGIC ||
        list->version != WIFI_CREDENTIAL_VERSION || list->record_size != sizeof(*list) ||
        list->count > MYBOT_WIFI_MAX_CREDENTIALS) {
        return -1;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (!mybot_wifi_credential_is_valid(&list->entries[i])) {
            return -1;
        }
    }
    return bk_set_env_enhance(WIFI_CREDENTIAL_KEY, list, sizeof(*list)) == EF_NO_ERR ? 0 : -1;
}

static int append_migrated_credential(mybot_wifi_credential_list_t *list, const uint8_t *ssid,
                                      const uint8_t *password) {
    if (!list || !string_is_terminated(ssid, WIFI_SSID_STR_LEN) ||
        !string_is_terminated(password, WIFI_PASSWORD_LEN)) {
        return -1;
    }

    size_t ssid_length = strlen((const char *)ssid);
    size_t password_length = strlen((const char *)password);
    if (!credential_lengths_are_valid(ssid_length, password_length)) {
        return -1;
    }

    mybot_wifi_credential_entry_t *entry = &list->entries[0];
    entry->ssid_length = (uint8_t)ssid_length;
    entry->password_length = (uint8_t)password_length;
    memcpy(entry->ssid, ssid, ssid_length + 1);
    memcpy(entry->password, password, password_length + 1);
    list->count = 1;
    return 0;
}

static int migrate_v1_credentials(mybot_wifi_credential_list_t *list) {
    mybot_wifi_credentials_v1_t saved = {0};
    int length = bk_get_env_enhance(WIFI_CREDENTIAL_KEY, &saved, sizeof(saved));

    if (length != (int)sizeof(saved) || saved.magic != WIFI_CREDENTIAL_MAGIC ||
        saved.version != WIFI_LEGACY_CREDENTIAL_VERSION ||
        saved.record_size != sizeof(saved) ||
        !credential_lengths_are_valid(saved.ssid_length, saved.password_length) ||
        saved.ssid[saved.ssid_length] != '\0' || saved.password[saved.password_length] != '\0') {
        return -1;
    }
    return append_migrated_credential(list, saved.ssid, saved.password);
}

static int migrate_bk_legacy_credentials(mybot_wifi_credential_list_t *list) {
    mybot_wifi_bk_legacy_credentials_t saved = {0};
    int length = bk_get_env_enhance(WIFI_BK_LEGACY_CREDENTIAL_KEY, &saved, sizeof(saved));

    if (length != (int)sizeof(saved) ||
        (saved.flag & WIFI_BK_LEGACY_CREDENTIAL_MAGIC) != WIFI_BK_LEGACY_CREDENTIAL_MAGIC ||
        (saved.flag & (1u << NETIF_IF_STA)) == 0) {
        return -1;
    }
    return append_migrated_credential(list, saved.sta_ssid, saved.sta_password);
}

int mybot_wifi_credentials_load(mybot_wifi_credential_list_t *list) {
    if (!list) {
        return -1;
    }

    mybot_wifi_credentials_init(list);
    int length = bk_get_env_enhance(WIFI_CREDENTIAL_KEY, list, sizeof(*list));
    bool valid = length == (int)sizeof(*list) && list->magic == WIFI_CREDENTIAL_MAGIC &&
                 list->version == WIFI_CREDENTIAL_VERSION &&
                 list->record_size == sizeof(*list) &&
                 list->count <= MYBOT_WIFI_MAX_CREDENTIALS;
    for (size_t i = 0; valid && i < list->count; ++i) {
        valid = mybot_wifi_credential_is_valid(&list->entries[i]);
    }
    if (valid) {
        CREDENTIAL_LOGI("credentials loaded: count=%u", (unsigned)list->count);
        return 0;
    }

    mybot_wifi_credentials_init(list);
    if (migrate_v1_credentials(list) < 0 && migrate_bk_legacy_credentials(list) < 0) {
        mybot_wifi_credentials_init(list);
        CREDENTIAL_LOGI("no saved credentials");
        return 0;
    }

    CREDENTIAL_LOGI("legacy credentials migrated: count=%u", (unsigned)list->count);
    if (mybot_wifi_credentials_save(list) < 0) {
        CREDENTIAL_LOGW("failed to persist migrated credentials");
    }
    return 0;
}

int mybot_wifi_credentials_add(mybot_wifi_credential_list_t *list, const char *ssid,
                               const char *password) {
    if (!list || !ssid || !password || list->count > MYBOT_WIFI_MAX_CREDENTIALS) {
        return -1;
    }
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    if (!credential_lengths_are_valid(ssid_length, password_length)) {
        return -1;
    }

    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->entries[i].ssid, ssid) == 0) {
            memset(list->entries[i].password, 0, sizeof(list->entries[i].password));
            memcpy(list->entries[i].password, password, password_length + 1);
            list->entries[i].password_length = (uint8_t)password_length;
            return 0;
        }
    }

    size_t move_count = list->count < MYBOT_WIFI_MAX_CREDENTIALS
                            ? list->count
                            : MYBOT_WIFI_MAX_CREDENTIALS - 1;
    if (move_count > 0) {
        memmove(&list->entries[1], &list->entries[0], move_count * sizeof(list->entries[0]));
    }
    memset(&list->entries[0], 0, sizeof(list->entries[0]));
    list->entries[0].ssid_length = (uint8_t)ssid_length;
    list->entries[0].password_length = (uint8_t)password_length;
    memcpy(list->entries[0].ssid, ssid, ssid_length + 1);
    memcpy(list->entries[0].password, password, password_length + 1);
    if (list->count < MYBOT_WIFI_MAX_CREDENTIALS) {
        ++list->count;
    }
    return 0;
}

int mybot_wifi_credentials_set_default(mybot_wifi_credential_list_t *list, size_t index) {
    if (!list || list->count > MYBOT_WIFI_MAX_CREDENTIALS || index >= list->count) {
        return -1;
    }
    if (index > 0) {
        mybot_wifi_credential_entry_t selected = list->entries[index];
        memmove(&list->entries[1], &list->entries[0], index * sizeof(list->entries[0]));
        list->entries[0] = selected;
    }
    return 0;
}

int mybot_wifi_credentials_delete(mybot_wifi_credential_list_t *list, size_t index) {
    if (!list || list->count > MYBOT_WIFI_MAX_CREDENTIALS || index >= list->count) {
        return -1;
    }
    if (index + 1 < list->count) {
        memmove(&list->entries[index], &list->entries[index + 1],
                (list->count - index - 1) * sizeof(list->entries[0]));
    }
    --list->count;
    memset(&list->entries[list->count], 0, sizeof(list->entries[0]));
    return 0;
}
