from pathlib import Path
p=Path('head/main/network/network_manager.c');s=p.read_text(encoding='utf8')
a=s.index('    cJSON *root=cJSON_Parse(resp);');b=s.index('\n}',a)
block=s[a:b];s=s[:a]+'    httpd_resp_sendstr(req, resp);\n    return ESP_OK;'+s[b:]
a=s.index('static esp_err_t rest_api_status_handler');b=s.index('\n}',a)
part=s[a:b].replace('    httpd_resp_sendstr(req, resp);\n    return ESP_OK;',block)
s=s[:a]+part+s[b:];p.write_text(s,encoding='utf8')
