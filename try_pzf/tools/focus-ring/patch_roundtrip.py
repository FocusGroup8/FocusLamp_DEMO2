from pathlib import Path
p=Path('head/main/network/network_manager.c');s=p.read_text(encoding='utf8');idx=s.index('static void ws_data_handler');s=s[:idx]+'''static void focus_clock_reply(int fd)
{
    char body[96];
    int n=snprintf(body,sizeof(body),"{\\"focus_clock_ms\\":%lu}",(unsigned long)(uint32_t)(esp_timer_get_time()/1000));
    if(fd>=0 && n>0) (void)message_queue_enqueue(fd,body,n,MSG_QUEUE_TYPE_TEXT);
}
'''+s[idx:];s=s.replace('if(cJSON_IsString(epoch)) focus_sync_source(msg->client_fd,epoch->valuestring);','if(cJSON_IsString(epoch)) { focus_sync_source(msg->client_fd,epoch->valuestring); focus_clock_reply(msg->client_fd); }');s=s.replace('        focus_sync_sample(msg->client_fd,cJSON_GetObjectItemCaseSensitive(focus_args,"focus"));','''    {
        focus_sync_sample(msg->client_fd,cJSON_GetObjectItemCaseSensitive(focus_args,"focus"));
        focus_clock_reply(msg->client_fd);
    }''');p.write_text(s,encoding='utf8')
p=Path('head/main/algo/focus_sync.c');s=p.read_text(encoding='utf8').replace('    sample_t next={0};','    sample_t next={0}; uint32_t echoed;');s=s.replace('!number(j,"sample_age_ms",&next.sample_age)) return;','''!number(j,"sample_age_ms",&next.sample_age)||!number(j,"head_clock_ms",&echoed)) return;
    uint32_t transit=(uint32_t)(esp_timer_get_time()/1000)-echoed;
    if(transit>3000)return;
    next.age=next.age>UINT32_MAX-transit?UINT32_MAX:next.age+transit;
    next.sample_age=next.sample_age>UINT32_MAX-transit?UINT32_MAX:next.sample_age+transit;''');p.write_text(s,encoding='utf8')
p=Path('tools/focus-ring/docker/main-client/focus_metadata.py');s=p.read_text(encoding='utf8').replace('# Explicit conservative WS budget; MCU adds its own residence/HTTP budget.','# MCU measures transport using its own echoed monotonic clock.').replace('+wait+500','+wait');p.write_text(s,encoding='utf8')
p=Path('tools/focus-ring/docker/main-client/main.py');s=p.read_text(encoding='utf8').replace('self.focus_sent_source = None','self.focus_sent_source = None\n        self.focus_head_clock = None',1);s=s.replace('                    self.focus_sent_source = None','                    self.focus_sent_source = None\n                    self.focus_head_clock = None');s=s.replace('                            logger.debug("algo ws response: %s", msg[:200])','''                            logger.debug("algo ws response: %s", msg[:200])
                            try:
                                clock=json.loads(msg).get('focus_clock_ms')
                                if isinstance(clock,int) and 0<=clock<=0xffffffff:
                                    self.focus_head_clock=clock
                            except (ValueError,AttributeError):
                                pass''');s=s.replace("                            self.focus_sent_source = focus['source_epoch']", "                            self.focus_sent_source = focus['source_epoch']\n                            self.focus_head_clock = None");s=s.replace('                        await self.algo_ws.send(json.dumps(payload))','''                        if focus:
                            if self.focus_head_clock is None:
                                payload['params']['arguments'].pop('focus',None)
                            else:
                                focus['head_clock_ms']=self.focus_head_clock
                        await self.algo_ws.send(json.dumps(payload))''');p.write_text(s,encoding='utf8')
p=Path('controller/components/focuslamp_bridge/src/focuslamp_bridge.c');s=p.read_text(encoding='utf8').replace('char response[384];\n  snprintf(response, sizeof(response),\n      "{\\"ok\\":%s,\\"effects_async', 'char response[768];\n  snprintf(response, sizeof(response),\n      "{\\"ok\\":%s,\\"effects_async');s=s.replace('"\\"remaining_sec\\":%lu,\\"paused\\":%s,\\"error\\":\\"%s\\","','''"\\"remaining_sec\\":%lu,\\"remaining_ms\\":%lu,\\"session_id\\":%lu,\\"revision\\":%lu,"
      "\\"completed\\":%s,\\"pause_reason\\":\\"%s\\",\\"paused\\":%s,\\"error\\":\\"%s\\","''');s=s.replace('(unsigned long)status.remaining_sec, status.paused ? "true" : "false",','''(unsigned long)status.remaining_sec, (unsigned long)status.remaining_ms,
      (unsigned long)status.session_id, (unsigned long)status.revision, status.completed ? "true":"false",
      controller_modes_pause_name(status.pause_reason), status.paused ? "true" : "false",''');p.write_text(s,encoding='utf8')
