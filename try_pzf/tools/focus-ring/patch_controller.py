from pathlib import Path
p=Path('controller/components/controller_modes/src/controller_modes.c')
s=p.read_text();s=s.replace('#include <stddef.h>','#include <stddef.h>\n#include <string.h>\n#include "esp_random.h"\n#include "focus_clock.h"')
s=s.replace('static int64_t s_elapsed_us;','static focus_clock_t s_focus;\nstatic uint32_t s_boot_id, s_input_token;')
a=s.index('    int64_t delta_us =',s.index('static bool advance_locked'));b=s.index('\nstatic void snapshot_locked',a)
s=s[:a]+'''    s_last_us = now_us;
    bool was_completed = s_focus.completed;
    focus_clock_settle(&s_focus, now_us);
    s_completed = s_focus.completed;
    if (!was_completed && s_completed) ++s_revision;
    return !was_completed && s_completed;
}

const char *controller_modes_pause_name(unsigned reason)
{
    static const char *names[] = {"waiting", "running", "distracted", "invalid", "data_stale", "voice", "completed"};
    return reason < sizeof(names)/sizeof(names[0]) ? names[reason] : "invalid";
}
''' +s[b:]
s=s.replace('int64_t remaining_us = (int64_t)s_duration_sec * 1000000 - s_elapsed_us;', 'int64_t remaining_us = s_focus.remaining_us;')
s=s.replace('.paused = s_mode == APP_STATE_FOCUS && s_voice_active,','.paused = s_mode == APP_STATE_FOCUS && !s_completed && (s_voice_active || !s_focus.rate_half),\n        .duration_ms = s_duration_sec * 1000,\n        .remaining_ms = (uint32_t)((remaining_us + 999) / 1000),\n        .controller_boot_id = s_boot_id, .session_id = s_focus.session,\n        .seq = ++s_focus.seq, .engagement_token = s_input_token,\n        .rate_half = s_voice_active || s_completed ? 0 : s_focus.rate_half,\n        .pause_reason = s_completed ? FOCUS_DONE : s_voice_active ? FOCUS_VOICE : s_focus.reason,')
s=s.replace('s_elapsed_us = 0;', 's_focus.remaining_us = 0; s_focus.active = false; s_focus.completed = false;')
s=s.replace('s_timer = timer; /* Retained', 's_boot_id = esp_random();\n        s_timer = timer; /* Retained')
s=s.replace('if (s_mode == APP_STATE_FOCUS) {\n        ret', 'if (s_mode == APP_STATE_FOCUS && !s_completed) {\n        ret')
s=s.replace('s_duration_sec = minutes * 60;\n        s_focus.remaining_us = 0; s_focus.active = false; s_focus.completed = false;', 's_duration_sec = minutes * 60;\n        focus_clock_start(&s_focus, s_duration_sec, s_last_us);\n        s_input_token = 0;')
s=s.replace('s_voice_active = active;','s_voice_active = active;\n    focus_clock_voice(&s_focus, active, esp_timer_get_time());')
s += '''
static int engagement_rate(const char *level)
{
    if (!strcmp(level, "HighlyEngaged") || !strcmp(level, "Engaged") || !strcmp(level,"高度参与") || !strcmp(level,"参与")) return 2;
    if (!strcmp(level, "BarelyEngaged") || !strcmp(level,"勉强参与")) return 1;
    if (!strcmp(level, "NotEngaged") || !strcmp(level,"未参与")) return 0;
    return -1;
}
esp_err_t controller_modes_engagement(uint32_t boot, uint32_t session, uint32_t token,
    bool bind, uint32_t sample, uint32_t inference, uint32_t age_ms, uint32_t sample_age_ms,
    bool valid, const char *level)
{
    int rate = engagement_rate(level ? level : "");
    portENTER_CRITICAL(&s_lock);
    int64_t now = esp_timer_get_time();
    advance_locked(now);
    if (!s_initialized || boot != s_boot_id || session != s_focus.session ||
        s_mode != APP_STATE_FOCUS || s_completed || (!bind && (!token || token != s_input_token))) {
        portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE;
    }
    if (bind) {
        focus_clock_bind(&s_focus, now);
        s_input_token = esp_random(); if (!s_input_token) s_input_token = 1;
    } else {
        focus_clock_sample(&s_focus, sample, inference, (int64_t)age_ms*1000,
                           (int64_t)sample_age_ms*1000, valid, rate, now);
    }
    controller_modes_status_t status; snapshot_locked(&status);
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "[FOCUS] session=%lu seq=%lu remaining_ms=%lu rate=%d/2 reason=%s age_ms=%lu",
        (unsigned long)status.session_id, (unsigned long)status.seq, (unsigned long)status.remaining_ms,
        status.rate_half, controller_modes_pause_name(status.pause_reason), (unsigned long)age_ms);
    return ESP_OK;
}
'''
p.write_text(s)
p=Path('controller/components/controller_modes/include/controller_modes.h');s=p.read_text().replace('    uint32_t revision;', '    uint32_t duration_ms, remaining_ms, controller_boot_id, session_id, seq, engagement_token;\n    int rate_half;\n    unsigned pause_reason;\n    uint32_t revision;');s=s.replace('const char *controller_modes_name', '''const char *controller_modes_pause_name(unsigned reason);
esp_err_t controller_modes_engagement(uint32_t boot, uint32_t session, uint32_t token,
    bool bind, uint32_t sample, uint32_t inference, uint32_t age_ms, uint32_t sample_age_ms,
    bool valid, const char *level);
const char *controller_modes_name''');p.write_text(s)
p=Path('controller/components/controller_modes/CMakeLists.txt');s=p.read_text().replace('common esp_timer freertos','common esp_timer freertos esp_hw_support');p.write_text(s)
