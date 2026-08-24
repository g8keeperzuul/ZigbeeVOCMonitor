/*
 * LOCAL FILE (not from Waveshare).
 *
 * Upstream's Debug.h routes the vendored driver's trace output to printf behind
 * a DEBUG define. Route it to the IDF logger instead, so it obeys the usual log
 * level and tag filtering rather than spamming the console unconditionally.
 */
#ifndef _DEBUG_H_
#define _DEBUG_H_

#include "esp_log.h"

#define Debug(__info, ...) ESP_LOGD("epaper", __info, ##__VA_ARGS__)

#endif
