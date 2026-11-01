from pathlib import Path
p=Path('controller/main/mode_http_api.c');s=p.read_text();s=s.replace('char response[768];','char response[1200];');s=s.replace('"{\\"ok\\":true,\\"mode\\":\\"%s\\",\\"app_state\\":%u,"', '''"{\\"duration_ms\\":%lu,\\"remaining_ms\\":%lu,\\"controller_boot_id\\":%lu,"
        "\\"session_id\\":%lu,\\"seq\\":%lu,\\"revision\\":%lu,\\"engagement_token\\":%lu,"
        "\\"rate_half\\":%d,\\"pause_reason\\":\\"%s\\","
        "\\"ok\\":true,\\"mode\\":\\"%s\\",\\"app_state\\":%u,"''');s=s.replace('        controller_modes_name(state.mode), (unsigned)state.mode,','''        (unsigned long)state.duration_ms, (unsigned long)state.remaining_ms,
        (unsigned long)state.controller_boot_id, (unsigned long)state.session_id,
        (unsigned long)state.seq, (unsigned long)state.revision, (unsigned long)state.engagement_token,
        state.rate_half, controller_modes_pause_name(state.pause_reason),
        controller_modes_name(state.mode), (unsigned)state.mode,''')
idx=s.index('esp_err_t mode_http_api_register')
s=s[:idx]+'''static bool json_u32(const cJSON *root, const char *key, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > UINT32_MAX || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (uint32_t)v->valuedouble; return true;
}
static esp_err_t engagement_handler(httpd_req_t *req)
{
    char body[512]; size_t got = 0;
    httpd_resp_set_type(req, "application/json");
    if (!req->content_len || req->content_len >= sizeof(body)) goto bad;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) goto bad;
        got += n;
    }
    body[got] = 0;
    if (memchr(body, 0, got)) goto bad;
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    uint32_t boot, session, token=0, sample=0, inference=0, age=0, sample_age=0;
    bool bind = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"bind"));
    const cJSON *level=cJSON_GetObjectItemCaseSensitive(root,"level");
    const cJSON *valid=cJSON_GetObjectItemCaseSensitive(root,"valid");
    bool ok = json_u32(root,"controller_boot_id",&boot) && json_u32(root,"session_id",&session);
    if (!bind) ok = ok && json_u32(root,"engagement_token",&token) &&
        json_u32(root,"sample_seq",&sample) && json_u32(root,"inference_seq",&inference) &&
        json_u32(root,"age_ms",&age) && json_u32(root,"sample_age_ms",&sample_age) &&
        cJSON_IsString(level) && cJSON_IsBool(valid);
    esp_err_t ret = ok ? controller_modes_engagement(boot,session,token,bind,sample,inference,
        age,sample_age,cJSON_IsTrue(valid),cJSON_IsString(level)?level->valuestring:"") : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    if (!ok) goto bad;
    if (ret != ESP_OK) {
        httpd_resp_set_status(req,"409 Conflict");
        return httpd_resp_sendstr(req,"{\\"ok\\":false,\\"error\\":\\"resync_required\\"}");
    }
    return mode_status_response(req);
bad:
    httpd_resp_set_status(req,"400 Bad Request");
    httpd_resp_set_hdr(req,"Connection","close");
    httpd_resp_sendstr(req,"{\\"ok\\":false,\\"error\\":\\"invalid engagement\\"}");
    return ESP_FAIL;
}

'''+s[idx:]
s=s.replace('    ESP_LOGI(TAG, "GET/POST /api/mode registered (state/countdown only)");', '''    const httpd_uri_t engagement_uri = {.uri="/api/mode/engagement", .method=HTTP_POST, .handler=engagement_handler};
    ret = ws_manager_server_register_uri(&engagement_uri);
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "Mode and engagement APIs registered");''');p.write_text(s)
p=Path('controller/main/lcd_status_ui.c');s=p.read_text().replace('a->paused != b->paused','a->pause_reason != b->pause_reason || a->paused != b->paused');s=s.replace('(status.paused ? "PAUSED" : "RUNNING")','(status.paused ? controller_modes_pause_name(status.pause_reason) : "RUNNING")');s=s.replace('pdMS_TO_TICKS(1000)','pdMS_TO_TICKS(500)');p.write_text(s)
