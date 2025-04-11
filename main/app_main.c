/* Wi-Fi Provisioning Manager Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>

#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>
#include <cJSON.h>

static const char *TAG = "app";

/* Signal Wi-Fi events on this event-group */
const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;

#define PROV_QR_VERSION "v1"
#define PROV_TRANSPORT_BLE "ble"

/* Handler cho custom endpoint "ble_transmit" */
esp_err_t get_mac_endpoint_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                   uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    if (inbuf && inlen > 0)
    {
        ESP_LOGI(TAG, "Received data on ble_transmit: %.*s", inlen, (char *)inbuf);

        cJSON *json = cJSON_ParseWithLength((const char *)inbuf, inlen);
        if (json == NULL)
        {
            ESP_LOGE(TAG, "Failed to parse JSON");
        }
        else
        {
            const cJSON *ip = cJSON_GetObjectItem(json, "ip");
            const cJSON *port = cJSON_GetObjectItem(json, "p");
            const cJSON *user = cJSON_GetObjectItem(json, "u");
            const cJSON *password = cJSON_GetObjectItem(json, "pw");

            if (cJSON_IsString(ip) && cJSON_IsNumber(port) &&
                cJSON_IsString(user) && cJSON_IsString(password))
            {
                ESP_LOGI(TAG, "Parsed JSON:");
                ESP_LOGI(TAG, "  IP      : %s", ip->valuestring);
                ESP_LOGI(TAG, "  Port    : %d", port->valueint);
                ESP_LOGI(TAG, "  User    : %s", user->valuestring);
                ESP_LOGI(TAG, "  Password: %s", password->valuestring);
            }
            else
            {
                ESP_LOGE(TAG, "Invalid JSON structure");
            }

            cJSON_Delete(json);
        }
    }

    // Lấy MAC address Wi-Fi STA
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    *outbuf = (uint8_t *)strdup(mac_str);
    if (*outbuf == NULL)
    {
        ESP_LOGE(TAG, "System out of memory");
        return ESP_ERR_NO_MEM;
    }
    *outlen = strlen(mac_str) + 1;

    return ESP_OK;
}

// /* Handler cho custom endpoint "custom-data" */
// esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
//                                    uint8_t **outbuf, ssize_t *outlen, void *priv_data)
// {
//     if (inbuf && inlen > 0)
//     {
//         ESP_LOGI(TAG, "Received data: %.*s", inlen, (char *)inbuf);

//         cJSON *json = cJSON_ParseWithLength((const char *)inbuf, inlen);
//         if (json == NULL)
//         {
//             ESP_LOGE(TAG, "Failed to parse JSON");
//         }
//         else
//         {
//             const cJSON *ip = cJSON_GetObjectItem(json, "ip");
//             const cJSON *port = cJSON_GetObjectItem(json, "p");
//             const cJSON *user = cJSON_GetObjectItem(json, "u");
//             const cJSON *password = cJSON_GetObjectItem(json, "pw");

//             if (cJSON_IsString(ip) && cJSON_IsNumber(port) &&
//                 cJSON_IsString(user) && cJSON_IsString(password))
//             {
//                 ESP_LOGI(TAG, "Parsed JSON:");
//                 ESP_LOGI(TAG, "  IP      : %s", ip->valuestring);
//                 ESP_LOGI(TAG, "  Port    : %d", port->valueint);
//                 ESP_LOGI(TAG, "  User    : %s", user->valuestring);
//                 ESP_LOGI(TAG, "  Password: %s", password->valuestring);
//             }
//             else
//             {
//                 ESP_LOGE(TAG, "Invalid JSON structure");
//             }

//             cJSON_Delete(json);
//         }
//     }

//     const char *response = "SUCCESS";
//     *outbuf = (uint8_t *)strdup(response);
//     if (*outbuf == NULL)
//     {
//         ESP_LOGE(TAG, "System out of memory");
//         return ESP_ERR_NO_MEM;
//     }
//     *outlen = strlen(response) + 1;

//     return ESP_OK;
// }

/* Event handler for catching system events */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
    static int retries;
#endif
    if (event_base == WIFI_PROV_EVENT)
    {
        switch (event_id)
        {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            break;
        case WIFI_PROV_CRED_RECV:
        {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received Wi-Fi credentials"
                          "\n\tSSID     : %s\n\tPassword : %s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
            break;
        }
        case WIFI_PROV_CRED_FAIL:
        {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                          "\n\tPlease reset to factory and retry provisioning",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
            retries++;
            if (retries >= CONFIG_EXAMPLE_PROV_MGR_MAX_RETRY_CNT)
            {
                ESP_LOGI(TAG, "Failed to connect with provisioned AP, reseting provisioned credentials");
                wifi_prov_mgr_reset_sm_state_on_failure();
                retries = 0;
            }
#endif
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
            retries = 0;
#endif
            break;
        case WIFI_PROV_END:
            /* De-initialize manager once provisioning is finished */
            wifi_prov_mgr_deinit();
            break;
        default:
            break;
        }
    }
    else if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
            esp_wifi_connect();
            break;
        default:
            break;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    }
    else if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT)
    {
        switch (event_id)
        {
        case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
            ESP_LOGI(TAG, "BLE transport: Connected!");
            break;
        case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
            ESP_LOGI(TAG, "BLE transport: Disconnected!");
            break;
        default:
            break;
        }
    }
    else if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT)
    {
        switch (event_id)
        {
        case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
            ESP_LOGI(TAG, "Secured session established!");
            break;
        case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
            ESP_LOGE(TAG, "Received invalid security parameters for establishing secure session!");
            break;
        case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
            ESP_LOGE(TAG, "Received incorrect username and/or PoP for establishing secure session!");
            break;
        default:
            break;
        }
    }
}

static void wifi_init_sta(void)
{
    /* Start Wi-Fi in station mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

void app_main(void)
{
    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /* NVS partition was truncated
         * and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());

        /* Retry nvs_flash_init */
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize TCP/IP */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize the event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();

    /* Register our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    /* Initialize Wi-Fi including netif with default config */
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {
        /* What is the Provisioning Scheme that we want ? */
        .scheme = wifi_prov_scheme_ble,

        /* Any default scheme specific event handler that you would
         * like to choose. Since our example application requires
         * BLE, we can choose to release the associated memory once
         * provisioning is complete, or not needed
         * (in case when device is already provisioned). Choosing
         * appropriate scheme specific event handler allows the manager
         * to take care of this automatically. */
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM};

    /* Initialize provisioning manager with the
     * configuration parameters set above */
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    bool provisioned = false;
#ifdef CONFIG_EXAMPLE_RESET_PROVISIONED
    wifi_prov_mgr_reset_provisioning();
#else
    /* Let's find out if the device is provisioned */
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));
#endif

    /* If device is not yet provisioned start provisioning service */
    if (!provisioned)
    {
        ESP_LOGI(TAG, "Starting provisioning");

        /* What is the Device Service Name that we want
         * This translates to:
         *     - device name when scheme is wifi_prov_scheme_ble
         */
        char service_name[12];
        get_device_service_name(service_name, sizeof(service_name));

        /* Set security level to 0 (no security) */
        wifi_prov_security_t security = WIFI_PROV_SECURITY_0;

        /* Do not use proof-of-possession for security 0 */
        const void *sec_params = NULL;

        /* What is the service key (không dùng cho BLE scheme) */
        const char *service_key = NULL;

        /* This step is useful for BLE scheme. This will set a custom 128 bit UUID
         * which will be included in the BLE advertisement and will correspond to
         * the primary GATT service that provides provisioning endpoints as GATT
         * characteristics. Each GATT characteristic will be formed using the primary
         * service UUID as base, with different auto assigned 12th and 13th bytes
         * (assume counting starts from 0th byte). The client side applications must
         * identify the endpoints by reading the User Characteristic Description
         * descriptor (0x2901) for each characteristic, which contains the endpoint
         * name of the characteristic */
        uint8_t custom_service_uuid[] = {
            /* LSB <---------------------------------------
             * ---------------------------------------> MSB */
            0xb4,
            0xdf,
            0x5a,
            0x1c,
            0x3f,
            0x6b,
            0xf4,
            0xbf,
            0xea,
            0x4a,
            0x82,
            0x03,
            0x04,
            0x90,
            0x1a,
            0x02,
        };

        /* If your build fails with linker errors at this point, then you may have
         * forgotten to enable the BT stack or BTDM BLE settings in the SDK (e.g. see
         * the sdkconfig.defaults in the example project) */
        wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);

        /* Tạo custom endpoint "ble_transmit" để trả về địa chỉ MAC */
        wifi_prov_mgr_endpoint_create("ble_transmit");

        /* Tạo custom endpoint "custom-data" */
        //wifi_prov_mgr_endpoint_create("custom-data");

        /* Do not stop and de-init provisioning even after success,
         * so that we can restart it later. */
#ifdef CONFIG_EXAMPLE_REPROVISIONING
        wifi_prov_mgr_disable_auto_stop(1000);
#endif
        /* Start provisioning service */
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, sec_params, service_name, service_key));

        /* Đăng ký handler cho custom endpoint "ble_transmit" */
        wifi_prov_mgr_endpoint_register("ble_transmit", get_mac_endpoint_handler, NULL);

        /* Đăng ký handler cho custom endpoint "custom-data" */
        //wifi_prov_mgr_endpoint_register("custom-data", custom_prov_data_handler, NULL);

        /* Thông tin provisioning */
        ESP_LOGI(TAG, "Provisioning Started. Use ESP-IDF Provisioning app or other tools");
        ESP_LOGI(TAG, "BLE Device Name: %s", service_name);
        ESP_LOGI(TAG, "Security: None (WIFI_PROV_SECURITY_0)");
        ESP_LOGI(TAG, "Transport: BLE");
        ESP_LOGI(TAG, "Custom endpoint 'ble_transmit' available - send '0' to get MAC address");
    }
    else
    {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

        /* We don't need the manager as device is already provisioned,
         * so let's release it's resources */
        wifi_prov_mgr_deinit();

        /* Start Wi-Fi station */
        wifi_init_sta();
    }

    /* Wait for Wi-Fi connection */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, true, true, portMAX_DELAY);

    /* Start main application now */
#if CONFIG_EXAMPLE_REPROVISIONING
    while (1)
    {
        for (int i = 0; i < 10; i++)
        {
            ESP_LOGI(TAG, "Hello World!");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        /* Resetting provisioning state machine to enable re-provisioning */
        wifi_prov_mgr_reset_sm_state_for_reprovision();

        /* Wait for Wi-Fi connection */
        xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, true, true, portMAX_DELAY);
    }
#else
    while (1)
    {
        ESP_LOGI(TAG, "Hello World!");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
#endif
}