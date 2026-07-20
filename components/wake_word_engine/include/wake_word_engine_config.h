#pragma once

#include "sdkconfig.h"

/*
 * Wake Word Engine Configuration
 * Maps build-time Kconfig options to code-level macros (COMP-002).
 */

#ifndef WAKE_WORD_ENGINE_CONFIG_H
#define WAKE_WORD_ENGINE_CONFIG_H

#define WAKE_WORD_ENGINE_ENABLE CONFIG_WAKE_WORD_ENGINE_ENABLE

#if (WAKE_WORD_ENGINE_ENABLE == 1)

/* AFE (Audio Front-End) configuration */
#define WAKE_WORD_ENGINE_AFE_MODE       CONFIG_WAKE_WORD_ENGINE_AFE_MODE
#define WAKE_WORD_ENGINE_AFE_TYPE       CONFIG_WAKE_WORD_ENGINE_AFE_TYPE

/* MultiNet language model selection */
#define WAKE_WORD_ENGINE_DEFAULT_LANG   CONFIG_WAKE_WORD_ENGINE_DEFAULT_LANG

/* Detection task configuration */
#define WAKE_WORD_ENGINE_TASK_STACK     CONFIG_WAKE_WORD_ENGINE_TASK_STACK
#define WAKE_WORD_ENGINE_TASK_PRIORITY  CONFIG_WAKE_WORD_ENGINE_TASK_PRIORITY
#define WAKE_WORD_ENGINE_TASK_CORE      CONFIG_WAKE_WORD_ENGINE_TASK_CORE

/* MultiNet detection threshold (0.0 ~ 0.9999) */
#define WAKE_WORD_ENGINE_DET_THRESHOLD  CONFIG_WAKE_WORD_ENGINE_DET_THRESHOLD

/* MultiNet detection timeout (ms) - duration for command phrase */
#define WAKE_WORD_ENGINE_DET_TIMEOUT    CONFIG_WAKE_WORD_ENGINE_DET_TIMEOUT

#endif /* WAKE_WORD_ENGINE_ENABLE */

#endif /* WAKE_WORD_ENGINE_CONFIG_H */
