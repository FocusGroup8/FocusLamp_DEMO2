/*
 * event_bus.h - Event bus interface for FocusLamp
 */

#pragma once
#ifndef __EVENT_BUS_H__
#define __EVENT_BUS_H__

#include <stdint.h>
#include "event_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Callback Type ===================== */
typedef void (*event_callback_t)(event_t *event, void *context);

/* ===================== Initialization ===================== */
int event_bus_init(void);

/* ===================== Subscription ===================== */
int event_bus_subscribe(event_type_t event_type, event_callback_t callback, void *context);
int event_bus_unsubscribe(event_type_t event_type, event_callback_t callback);

/* ===================== Publishing ===================== */
int event_bus_publish(event_t *event);

/* ===================== Utility ===================== */
int event_bus_publish_simple(event_type_t type);
uint32_t event_bus_get_timestamp(void);

#ifdef __cplusplus
}
#endif

#endif /* __EVENT_BUS_H__ */