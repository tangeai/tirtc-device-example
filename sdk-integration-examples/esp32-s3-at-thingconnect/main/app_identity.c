#include "app_identity.h"

#include <stdio.h>

#include "esp_mac.h"

esp_err_t app_identity_read(char *client_id,
                            size_t client_id_size,
                            char *mac_text,
                            size_t mac_text_size)
{
    if (client_id == NULL || client_id_size < APP_CLIENT_ID_SIZE ||
        mac_text == NULL || mac_text_size < APP_MAC_TEXT_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        client_id[0] = '\0';
        mac_text[0] = '\0';
        return err;
    }

    int client_length = snprintf(client_id,
                                 client_id_size,
                                 "%02X%02X%02X%02X%02X%02X",
                                 mac[0],
                                 mac[1],
                                 mac[2],
                                 mac[3],
                                 mac[4],
                                 mac[5]);
    int mac_length = snprintf(mac_text,
                              mac_text_size,
                              "%02X:%02X:%02X:%02X:%02X:%02X",
                              mac[0],
                              mac[1],
                              mac[2],
                              mac[3],
                              mac[4],
                              mac[5]);
    if (client_length != APP_CLIENT_ID_SIZE - 1 ||
        mac_length != APP_MAC_TEXT_SIZE - 1) {
        client_id[0] = '\0';
        mac_text[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
