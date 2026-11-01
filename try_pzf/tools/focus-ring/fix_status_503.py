from pathlib import Path
p=Path('controller/main/mode_http_api.c');s=p.read_text(encoding='utf8').replace('if(controller_modes_get_status(&s)!=ESP_OK) return httpd_resp_send_err(req,HTTPD_503_SERVICE_UNAVAILABLE,"modes not ready");','''if(controller_modes_get_status(&s)!=ESP_OK) {
        httpd_resp_set_status(req,"503 Service Unavailable");
        return httpd_resp_sendstr(req,"{\\"ok\\":false,\\"error\\":\\"modes not ready\\"}");
    }''');p.write_text(s,encoding='utf8')
