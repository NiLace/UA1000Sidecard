/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * meters.h - peak levels of the UA-1000's audio, read through JACK
 * (meters.c)
 */
#ifndef UA1000_GUI_METERS_H
#define UA1000_GUI_METERS_H

#include <stdbool.h>

#define METERS_MAX_CHANNELS 12 /* the card's capture side has 12 */
#define METERS_MAX_SOURCES (2 * METERS_MAX_CHANNELS)

enum meter_source_kind {
	METER_CAPTURE, /* WAVE IN: what the computer records */
	METER_PLAYBACK, /* WAVE OUT: what the computer plays (PipeWire) */
};

struct meters;

/* NULL without libjack, without a JACK server (PipeWire's or jackd) or
 * without the UA-1000's ports: no meters, nothing else changes. */
struct meters *meters_open(void);
/* Close the client and unload libjack; NULL is ignored. */
void meters_close(struct meters *m);
/* Whether a JACK port alias "alsa_pcm:<device>:..." names the ALSA card
 * index or id ("hw:2", "hw:UA1000,0"); true for an alias that is not an
 * ALSA one. */
bool meters_alsa_alias_is_card(const char *alias, int index, const char *id);
/* Connect again any meter the server has disconnected: the device was off
 * or unplugged and its ports came back.  Call it from time to time. */
void meters_reconnect(struct meters *m);
/* False once the JACK server has gone away (or m is NULL): close it, and
 * open again later. */
bool meters_alive(struct meters *m);
/*
 * The highest absolute sample of a card channel (0-based) since the last
 * call, linear (1.0 = 0 dBFS); *available says whether that channel is
 * metered at all.
 */
float meters_take_peak(struct meters *m, enum meter_source_kind kind,
		       unsigned int channel, bool *available);

#endif /* UA1000_GUI_METERS_H */
