/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000_midi.h - the UA-1000's MIDI port, through ALSA rawmidi
 */
#ifndef UA1000_MIDI_H
#define UA1000_MIDI_H

#include "ua1000_proto.h"

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct ua1000_midi;

/*
 * Find the UA-1000's control port and write its name ("hw:N,D,S") to port.
 * The card is recognised by the name the snd-ua101 driver gives it,
 * "UA-1000", and the port by the name usbmidi gives cable 1, "UA-1000
 * Control".  Returns 0, -ENODEV when no such card is present, or -ENXIO
 * when the card has no control port (a driver without the cable 1 fix).
 */
int ua1000_midi_find(char *port, size_t len);

/*
 * The sample rate the device runs at, as the snd-ua101 driver reports it in
 * the card's long name: "EDIROL UA-1000 (serial ...), 48000 Hz at ...".
 * Returns 0, or -ENODEV.
 */
int ua1000_midi_card_rate(unsigned int *hz);

/* The UA-1000's ALSA card: its index and its id ("UA1000").  0 or -ENODEV. */
int ua1000_midi_card(int *index, char *id, size_t len);

/* The parser behind ua1000_midi_card_rate(), for testing. */
bool ua1000_midi_rate_from_longname(const char *longname, unsigned int *hz);

/* Print every rawmidi output port, marking the UA-1000's control port. */
void ua1000_midi_list(FILE *out);

/*
 * Open a port.  With with_input, the device's messages can be read with
 * ua1000_midi_receive().  Returns 0 or a negative errno.
 */
int ua1000_midi_open(struct ua1000_midi **midi, const char *port,
		     bool with_input);
void ua1000_midi_close(struct ua1000_midi *midi);

/* How long a send waits for the device to take bytes, at most. */
#define UA1000_MIDI_WRITE_TIMEOUT_MS 1000

/*
 * Queue messages for the device.  Returns 0 or a negative errno; -ETIMEDOUT
 * when the device took nothing for UA1000_MIDI_WRITE_TIMEOUT_MS (a message
 * may then be cut short: close the port, a new open starts clean).
 */
int ua1000_midi_send(struct ua1000_midi *midi, const struct ua1000_msg *msg,
		     size_t count);

/*
 * Wait until everything queued has left for the device:
 * UA1000_MIDI_WRITE_TIMEOUT_MS at most, then the rest is dropped and
 * -ETIMEDOUT returned.  ua1000_midi_close() never waits on the device.
 */
int ua1000_midi_drain(struct ua1000_midi *midi);

/*
 * The descriptors to poll for input, for an event loop.  Returns how many
 * were written to fds, at most max.
 */
int ua1000_midi_poll_descriptors(struct ua1000_midi *midi, struct pollfd *fds,
				 int max);

/*
 * Read what the device has sent, without blocking, and call handler for each
 * well-formed DT1 message.  Returns 0, or a negative errno when the port has
 * failed (the device was unplugged, for instance).
 */
typedef void ua1000_midi_handler(const struct ua1000_dt1 *dt1, void *data);

int ua1000_midi_receive(struct ua1000_midi *midi, ua1000_midi_handler *handler,
			void *data);

/* A description of a negative errno from these functions. */
const char *ua1000_midi_strerror(int err);

#endif /* UA1000_MIDI_H */
