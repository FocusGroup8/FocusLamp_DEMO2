from pathlib import Path
p=Path('head/main/algo/focus_sync.c');s=p.read_text().replace('char bound_host[16]={0}; uint16_t bound_port=0;','char bound_host[16]={0}; uint16_t bound_port=0; bool active=false;');s=s.replace('if(boot && data.present','if(active && boot && data.present').replace('}else if(boot && token','}else if(active && boot && token');s=s.replace('boot=next.boot;session=next.session;','boot=next.boot;session=next.session;active=next.focus && !next.completed;');s=s.replace('snprintf(sample.level,sizeof(sample.level),"%s",level->valuestring);','''const char *name=level->valuestring;
        if(strcmp(name,"HighlyEngaged") && strcmp(name,"Engaged") && strcmp(name,"BarelyEngaged") && strcmp(name,"NotEngaged")) name="Unknown";
        snprintf(sample.level,sizeof(sample.level),"%s",name);''');p.write_text(s)
p=Path('head/main/CMakeLists.txt');s=p.read_text().replace('"algo/algo_result_manager.c")','"algo/algo_result_manager.c" "algo/focus_sync.c")');p.write_text(s)
p=Path('head/main/network/network_manager.c');s=p.read_text().replace('#include "algo_result_manager.h"','#include "algo_result_manager.h"\n#include "focus_sync.h"');marker='    /* Check for mode.set notification';idx=s.index(marker);s=s[:idx]+'''    /* Explicit source handshake binds algorithm metadata to this WS client.
     * Ordinary /algo and HTTP results without metadata do not start countdown. */
    cJSON *focus_rpc=cJSON_Parse(request_buf);
    const cJSON *focus_method=cJSON_GetObjectItemCaseSensitive(focus_rpc,"method");
    const cJSON *focus_params=cJSON_GetObjectItemCaseSensitive(focus_rpc,"params");
    if(cJSON_IsString(focus_method) && !strcmp(focus_method->valuestring,"focus.source")) {
        const cJSON *epoch=cJSON_GetObjectItemCaseSensitive(focus_params,"source_epoch");
        if(cJSON_IsString(epoch)) focus_sync_source(msg->client_fd,epoch->valuestring);
        cJSON_Delete(focus_rpc);free(request_buf);return;
    }
    const cJSON *focus_args=cJSON_GetObjectItemCaseSensitive(focus_params,"arguments");
    const cJSON *tool_name=cJSON_GetObjectItemCaseSensitive(focus_params,"name");
    if(cJSON_IsString(focus_method) && !strcmp(focus_method->valuestring,"tools/call") &&
       cJSON_IsString(tool_name) && !strcmp(tool_name->valuestring,"algorithm.result"))
        focus_sync_sample(msg->client_fd,cJSON_GetObjectItemCaseSensitive(focus_args,"focus"));
    cJSON_Delete(focus_rpc);

'''+s[idx:];s=s.replace('    ret = algo_result_manager_init();','    if (focus_sync_start() != ESP_OK) ESP_LOGE(TAG,"Focus sync worker unavailable");\n    ret = algo_result_manager_init();');p.write_text(s)
p=Path('head/main/algo/algo_result_manager.c');s=p.read_text().replace('Focus mode entered, 30min countdown started','Focus mode entered; countdown is owned by controller');p.write_text(s)
