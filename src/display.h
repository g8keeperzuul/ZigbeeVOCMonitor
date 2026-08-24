#pragma once

#include "sensor.h"

/*
 * Waveshare 2.13" e-Paper (V4) readout: the VOC index large, with PM2.5,
 * temperature and humidity beneath.
 *
 * Runs on its own task because a refresh blocks for about a second. Feed it
 * with display_publish() from wherever readings are produced; it decides when
 * the panel is actually worth waking.
 */

/* Creates the mailbox and the display task. Panel bring-up happens on that task,
 * so a missing or faulty display costs nothing but the task itself. */
esp_err_t display_start(void);

/* Hands over the newest reading. Non-blocking, safe from any task, and cheap --
 * it overwrites a one-deep mailbox rather than queueing. */
void display_publish(const sen55_reading_t *reading);
