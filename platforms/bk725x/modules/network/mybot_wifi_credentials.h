/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_WIFI_CREDENTIALS_H_
#define MYBOT_WIFI_CREDENTIALS_H_

#include <modules/wifi_types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MYBOT_WIFI_MAX_CREDENTIALS 10

typedef struct {
    uint8_t ssid_length;
    uint8_t password_length;
    uint8_t reserved[2];
    char ssid[WIFI_SSID_STR_LEN];
    char password[WIFI_PASSWORD_LEN];
} mybot_wifi_credential_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint8_t count;
    uint8_t reserved[3];
    mybot_wifi_credential_entry_t entries[MYBOT_WIFI_MAX_CREDENTIALS];
} mybot_wifi_credential_list_t;

bool mybot_wifi_credential_is_valid(const mybot_wifi_credential_entry_t *entry);
void mybot_wifi_credentials_init(mybot_wifi_credential_list_t *list);
int mybot_wifi_credentials_load(mybot_wifi_credential_list_t *list);
int mybot_wifi_credentials_save(const mybot_wifi_credential_list_t *list);
int mybot_wifi_credentials_add(mybot_wifi_credential_list_t *list, const char *ssid,
                               const char *password);
int mybot_wifi_credentials_set_default(mybot_wifi_credential_list_t *list, size_t index);
int mybot_wifi_credentials_delete(mybot_wifi_credential_list_t *list, size_t index);

#endif /* MYBOT_WIFI_CREDENTIALS_H_ */
