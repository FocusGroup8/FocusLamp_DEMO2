#include "device_info_api.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "websocket_manager.h"

static const char *TAG = "DEVICE_INFO_API";

static esp_err_t device_info_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root ||
        !cJSON_AddStringToObject(root, "protocol", "FOCUSLAMP/1") ||
        !cJSON_AddStringToObject(root, "role", "controller") ||
        !cJSON_AddStringToObject(root, "kit_id", CONFIG_FOCUSLAMP_KIT_ID) ||
        !cJSON_AddNumberToObject(root, "http_port", CONFIG_WS_MANAGER_SERVER_PORT)) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    char *response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!response) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        ret = httpd_resp_sendstr(req, response);
    }
    cJSON_free(response);
    return ret;
}

esp_err_t device_info_api_register(void)
{
    const httpd_uri_t device_info_uri = {
        .uri = "/api/device/info",
        .method = HTTP_GET,
        .handler = device_info_handler,
    };

    esp_err_t ret = ws_manager_server_register_uri(&device_info_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register GET /api/device/info failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "GET /api/device/info registered");
    return ESP_OK;
}
