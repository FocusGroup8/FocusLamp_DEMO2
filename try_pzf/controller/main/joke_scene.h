#pragma once
#include "esp_err.h"
esp_err_t joke_scene_init(void);
void joke_scene_note_user(const char *text);
void joke_scene_note_tts_start(void);
void joke_scene_note_sentence(void);
void joke_scene_note_end(void);
void joke_scene_note_vision_accepted(void);
void joke_scene_note_companion_ended(void);
void joke_scene_note_distraction(void);
