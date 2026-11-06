/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000_midi.c - the UA-1000's MIDI port, through ALSA rawmidi
 *
 * The only module that talks to ALSA.  It finds the UA-1000's card and its
 * control port (USB-MIDI cable 1) by name, reads the running sample rate from
 * the card's long name, opens the port, and carries the messages built by
 * ua1000_proto.c to the device and the device's messages back.
 *
 * The port is opened non-blocking and stays so: every wait here is bounded by
 * UA1000_MIDI_WRITE_TIMEOUT_MS, because a caller may be a GUI's main loop and
 * a device that stops taking bytes must not freeze it.  For the same reason a
 * close drops what the device has not taken instead of letting the kernel
 * drain it.
 *
 * Incoming bytes go through a struct ua1000_reader kept in the handle, so a
 * message split across reads still arrives whole.  The module does not
 * decode messages (ua1000_decode() does) nor keep any state of the device.
 *
 * Used by ua1000ctl.c and the GUI under src/gui/.
 */
#include "ua1000_midi.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The card name the snd-ua101 driver registers for the UA-1000. */
#define CARD_NAME "UA-1000"

/*
 * An open port.  in is NULL for a port opened without input; reader holds a
 * message whose bytes have not all arrived yet.
 */
struct ua1000_midi {
	snd_rawmidi_t *in;
	snd_rawmidi_t *out;
	struct ua1000_reader reader;
};

/*
 * What one card is, read once per card.  ctl, longname and id are valid only
 * during the visitor's call: each_card() closes the card and reuses the info.
 */
struct card {
	int index;
	snd_ctl_t *ctl;
	const char *longname;
	const char *id;
	bool is_ua1000;
};

/* Call visit for every card; stop when it returns true. */
typedef bool card_visitor(const struct card *card, void *data);

/*
 * Open the control interface of each card in turn and pass what the card is
 * to visit, until visit returns true or the cards run out.  A card that
 * cannot be opened or queried is skipped; if the info cannot be allocated,
 * nothing is visited.
 */
static void each_card(card_visitor *visit, void *data)
{
	snd_ctl_card_info_t *info;
	int index = -1;
	bool stop = false;

	if (snd_ctl_card_info_malloc(&info) < 0)
		return;
	while (!stop && snd_card_next(&index) == 0 && index >= 0) {
		char name[32];
		struct card card = { .index = index };

		snprintf(name, sizeof(name), "hw:%d", index);
		if (snd_ctl_open(&card.ctl, name, 0) < 0)
			continue;
		if (snd_ctl_card_info(card.ctl, info) == 0) {
			card.longname = snd_ctl_card_info_get_longname(info);
			card.id = snd_ctl_card_info_get_id(info);
			card.is_ua1000 =
				strcmp(snd_ctl_card_info_get_name(info),
				       CARD_NAME) == 0;
			stop = visit(&card, data);
		}
		snd_ctl_close(card.ctl);
	}
	snd_ctl_card_info_free(info);
}

/*
 * The UA-1000's USB-MIDI interface has two cables: 0 for the MIDI jacks and
 * 1 for the control messages.  The kernel's usbmidi names the substream of
 * cable 1 "<card> Control".  A driver that exposes only one cable gives no
 * control port.
 */
#define CONTROL_SUFFIX " Control"

/* Call visit for every output substream of every rawmidi device of a card. */
typedef bool substream_visitor(const struct card *card, int device,
			       unsigned int subdevice, const char *name,
			       void *data);

/*
 * Walk the output substreams of every rawmidi device of card until visit
 * returns true; returns whether one did.  A device's subdevice count is known
 * only after its first query, hence count starts at 1.  A device without an
 * output stream fails that query and is skipped.
 */
static bool each_substream(const struct card *card, substream_visitor *visit,
			   void *data)
{
	snd_rawmidi_info_t *info;
	int device = -1;
	bool stop = false;

	if (snd_rawmidi_info_malloc(&info) < 0)
		return false;
	while (!stop && snd_ctl_rawmidi_next_device(card->ctl, &device) == 0 &&
	       device >= 0) {
		unsigned int count = 1;

		snd_rawmidi_info_set_device(info, (unsigned int)device);
		snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
		for (unsigned int sub = 0; !stop && sub < count; sub++) {
			snd_rawmidi_info_set_subdevice(info, sub);
			if (snd_ctl_rawmidi_info(card->ctl, info) < 0)
				break;
			count = snd_rawmidi_info_get_subdevices_count(info);
			stop = visit(card, device, sub,
				     snd_rawmidi_info_get_subdevice_name(info),
				     data);
		}
	}
	snd_rawmidi_info_free(info);
	return stop;
}

/*
 * Whether a substream name is a control port's: CONTROL_SUFFIX after a
 * non-empty card name.
 */
static bool is_control(const char *name)
{
	size_t len = strlen(name), suffix = strlen(CONTROL_SUFFIX);

	return len > suffix && strcmp(name + len - suffix, CONTROL_SUFFIX) == 0;
}

/* What ua1000_midi_find() looks for, and what it found. */
struct find {
	char *port;
	size_t len;
	bool card_found;
	bool found;
};

/* Substream visitor: name the first control port in f->port, and stop. */
static bool find_control(const struct card *card, int device,
			 unsigned int subdevice, const char *name, void *data)
{
	struct find *f = data;

	if (!is_control(name))
		return false;
	snprintf(f->port, f->len, "hw:%d,%d,%u", card->index, device,
		 subdevice);
	f->found = true;
	return true;
}

/*
 * Card visitor: search a UA-1000's substreams, pass every other card.  The
 * walk stops only once a control port is found, so a second UA-1000 is
 * searched when the first has none.
 */
static bool find_visitor(const struct card *card, void *data)
{
	struct find *f = data;

	if (!card->is_ua1000)
		return false;
	f->card_found = true;
	return each_substream(card, find_control, data);
}

/* The first control port of a UA-1000; see the header for the errors. */
int ua1000_midi_find(char *port, size_t len)
{
	struct find f = { .port = port, .len = len };

	each_card(find_visitor, &f);
	if (f.found)
		return 0;
	return f.card_found ? -ENXIO : -ENODEV;
}

/* Parse the rate out of the driver's long name; see the header. */
bool ua1000_midi_rate_from_longname(const char *longname, unsigned int *hz)
{
	/* The serial number is in parentheses; the rate follows them. */
	const char *p = strstr(longname, "), ");
	char *end;
	unsigned long rate;

	if (!p)
		return false;
	p += 3;
	if (*p < '0' || *p > '9')
		return false;
	errno = 0;
	rate = strtoul(p, &end, 10);
	if (errno || strncmp(end, " Hz", 3) != 0 || rate == 0 || rate > 1000000)
		return false;
	*hz = (unsigned int)rate;
	return true;
}

/* What ua1000_midi_card_rate() found. */
struct rate {
	unsigned int hz;
	bool found;
};

/* Card visitor: the rate of the first UA-1000 whose long name parses. */
static bool rate_visitor(const struct card *card, void *data)
{
	struct rate *r = data;

	if (!card->is_ua1000)
		return false;
	r->found = ua1000_midi_rate_from_longname(card->longname, &r->hz);
	return r->found; /* a name that does not parse: try the next unit */
}

/*
 * The first UA-1000's running rate; -ENODEV also when its long name carries
 * no rate.
 */
int ua1000_midi_card_rate(unsigned int *hz)
{
	struct rate r = { 0 };

	each_card(rate_visitor, &r);
	if (!r.found)
		return -ENODEV;
	*hz = r.hz;
	return 0;
}

/* What ua1000_midi_card() found. */
struct card_found {
	int index;
	char *id;
	size_t len;
	bool found;
};

/* Card visitor: take the first UA-1000's index and id, and stop. */
static bool card_visitor_ua1000(const struct card *card, void *data)
{
	struct card_found *f = data;

	if (!card->is_ua1000)
		return false;
	f->index = card->index;
	snprintf(f->id, f->len, "%s", card->id ? card->id : "");
	f->found = true;
	return true;
}

/* The first UA-1000 card; see the header. */
int ua1000_midi_card(int *index, char *id, size_t len)
{
	struct card_found f = { .id = id, .len = len };

	each_card(card_visitor_ua1000, &f);
	if (!f.found)
		return -ENODEV;
	*index = f.index;
	return 0;
}

/*
 * Substream visitor: print one port to the FILE in data, marking the
 * UA-1000's control port.  Never stops the walk.
 */
static bool list_substream(const struct card *card, int device,
			   unsigned int subdevice, const char *name, void *data)
{
	fprintf((FILE *)data, "hw:%d,%d,%u\t%s%s\n", card->index, device,
		subdevice, name,
		card->is_ua1000 && is_control(name) ? "\t(UA-1000 control)"
						    : "");
	return false;
}

/* Card visitor: list the card's output ports, then go on to the next. */
static bool list_visitor(const struct card *card, void *data)
{
	each_substream(card, list_substream, data);
	return false;
}

/* List every rawmidi output port of every card; see the header. */
void ua1000_midi_list(FILE *out)
{
	each_card(list_visitor, out);
}

/* Open port, with its input when asked; see the header. */
int ua1000_midi_open(struct ua1000_midi **midi, const char *port,
		     bool with_input)
{
	struct ua1000_midi *m = calloc(1, sizeof(*m));
	int err;

	if (!m)
		return -ENOMEM;
	ua1000_reader_init(&m->reader);
	/*
	 * One program at a time: a second opener gets -EBUSY.  (Sharing the
	 * output with SND_RAWMIDI_APPEND is not an option: alsa-lib sets the
	 * parameters of every port it opens, which the kernel refuses for a
	 * shared append output, and alsa-lib asserts on that failure.)  The
	 * default buffer holds the largest message, 268 bytes, many times.
	 *
	 * Opened non-blocking: a blocking open of a busy port does not fail,
	 * the kernel waits until the port is free (a second program hung on
	 * the device while another held the port).  The output stays
	 * non-blocking too: ua1000_midi_send() waits for room itself, for
	 * UA1000_MIDI_WRITE_TIMEOUT_MS at most, as a blocking write would hold
	 * a GUI's main loop for as long as the device takes no bytes.
	 */
	err = snd_rawmidi_open(with_input ? &m->in : NULL, &m->out, port,
			       SND_RAWMIDI_NONBLOCK);
	if (err < 0)
		goto fail;
	/*
	 * Start the input now: the kernel starts a rawmidi input only at its
	 * first read or poll, and what the device sends before that is lost.
	 * Observed: a GUI that asked for the device's state as it connected,
	 * and first polled after its window was built, never got the answer;
	 * the same request sent once its main loop ran got it in 3-32 ms.
	 * A poll that does not wait starts it, and consumes nothing.
	 */
	if (m->in) {
		struct pollfd fds[4];
		int n = snd_rawmidi_poll_descriptors(m->in, fds, 4);

		if (n > 0)
			(void)poll(fds, (nfds_t)n, 0);
	}
	*midi = m;
	return 0;

fail:
	ua1000_midi_close(m);
	return err;
}

/*
 * 1 while bytes wait in the output buffer, 0 when it is empty, -errno.  Bytes
 * wait while the room available is less than the whole buffer.  Close and
 * drain use it instead of the kernel's drain, which may wait for seconds.
 */
static int output_pending(snd_rawmidi_t *out)
{
	snd_rawmidi_params_t *params = NULL;
	snd_rawmidi_status_t *status = NULL;
	int err = snd_rawmidi_params_malloc(&params);

	if (err >= 0)
		err = snd_rawmidi_status_malloc(&status);
	if (err >= 0)
		err = snd_rawmidi_params_current(out, params);
	if (err >= 0)
		err = snd_rawmidi_status(out, status);
	if (err >= 0)
		err = snd_rawmidi_status_get_avail(status) <
		      snd_rawmidi_params_get_buffer_size(params);
	if (status)
		snd_rawmidi_status_free(status);
	if (params)
		snd_rawmidi_params_free(params);
	return err;
}

/*
 * Closing an output makes the kernel drain it first, whatever the handle's
 * mode: with a device that has stopped taking bytes, a close waited 10 s on
 * the kernels that predate the drain's stall detection (1 s on later ones),
 * and froze the GUI that called it.  What the device has not taken by now
 * will not reach it: drop it, and the close returns at once.
 */
void ua1000_midi_close(struct ua1000_midi *midi)
{
	if (!midi)
		return;
	if (midi->out) {
		if (output_pending(midi->out) != 0)
			snd_rawmidi_drop(midi->out);
		snd_rawmidi_close(midi->out);
	}
	if (midi->in)
		snd_rawmidi_close(midi->in);
	free(midi);
}

/*
 * The output buffer is full: wait until the device takes some of it.  A
 * device that takes nothing for UA1000_MIDI_WRITE_TIMEOUT_MS is reported,
 * -ETIMEDOUT, rather than waited for: the caller is a GUI's main loop.
 * Returns 0 once there is room, -ENODEV when the port reports an error or a
 * hang-up (the device was unplugged, for instance), or a negative errno.
 */
static int wait_for_room(struct ua1000_midi *midi)
{
	struct pollfd fds[4];
	unsigned short revents = 0;
	int n = snd_rawmidi_poll_descriptors(midi->out, fds, 4);
	int r;

	if (n <= 0)
		return n < 0 ? n : -EIO;
	do
		r = poll(fds, (nfds_t)n, UA1000_MIDI_WRITE_TIMEOUT_MS);
	while (r < 0 && errno == EINTR);
	if (r < 0)
		return -errno;
	if (r == 0)
		return -ETIMEDOUT;
	r = snd_rawmidi_poll_descriptors_revents(midi->out, fds,
						 (unsigned int)n, &revents);
	if (r < 0)
		return r;
	if (revents & (POLLERR | POLLHUP | POLLNVAL))
		return -ENODEV;
	return 0;
}

/* Write count messages whole, in order; see the header. */
int ua1000_midi_send(struct ua1000_midi *midi, const struct ua1000_msg *msg,
		     size_t count)
{
	for (size_t i = 0; i < count; i++) {
		size_t done = 0;

		while (done < msg[i].len) {
			ssize_t n = snd_rawmidi_write(midi->out,
						      msg[i].bytes + done,
						      msg[i].len - done);

			if (n == -EINTR)
				continue;
			/* nothing taken: no room, as -EAGAIN (else the
			 * loop would spin on a write that returns 0) */
			if (n == -EAGAIN || n == 0) {
				int err = wait_for_room(midi);

				if (err != 0)
					return err;
				continue;
			}
			if (n < 0)
				return (int)n;
			done += (size_t)n;
		}
	}
	return 0;
}

/*
 * Wait until the output buffer is empty, UA1000_MIDI_WRITE_TIMEOUT_MS at most,
 * by polling its status: the kernel's own drain (snd_rawmidi_drain) waits
 * up to 10 s for a device that stopped taking bytes.  On a timeout the
 * rest is dropped, so the close that follows does not wait either.  The
 * timeout counts the 10 ms steps slept, not the time spent in between.
 */
int ua1000_midi_drain(struct ua1000_midi *midi)
{
	const struct timespec step = { 0, 10000000L }; /* 10 ms */
	struct timespec start, now;

	/* the time that passed, not the sleeps counted: a call between them
	 * takes its own */
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		int pending = output_pending(midi->out);

		if (pending <= 0)
			return pending;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - start.tv_sec) * 1000 +
			    (now.tv_nsec - start.tv_nsec) / 1000000 >=
		    UA1000_MIDI_WRITE_TIMEOUT_MS) {
			snd_rawmidi_drop(midi->out);
			return -ETIMEDOUT;
		}
		nanosleep(&step, NULL);
	}
}

/* The input's descriptors; none for a port opened without input. */
int ua1000_midi_poll_descriptors(struct ua1000_midi *midi, struct pollfd *fds,
				 int max)
{
	if (!midi->in || max <= 0)
		return 0;
	return snd_rawmidi_poll_descriptors(midi->in, fds, (unsigned int)max);
}

/*
 * Read until the input would block, handing each complete, well-formed DT1
 * to handler.  Anything else (another SysEx, a bad checksum) is dropped
 * without a word.
 */
int ua1000_midi_receive(struct ua1000_midi *midi, ua1000_midi_handler *handler,
			void *data)
{
	uint8_t buf[256];

	if (!midi->in)
		return 0;
	for (;;) {
		ssize_t n = snd_rawmidi_read(midi->in, buf, sizeof(buf));

		if (n == -EAGAIN || n == 0)
			return 0;
		if (n == -EINTR)
			continue;
		if (n < 0)
			return (int)n;
		for (ssize_t i = 0; i < n; i++) {
			struct ua1000_dt1 dt1;

			if (ua1000_reader_feed(&midi->reader, buf[i]) &&
			    ua1000_parse_dt1(midi->reader.buf, midi->reader.len,
					     &dt1) == 0)
				handler(&dt1, data);
		}
	}
}

/* A macro's value as a string literal (for the messages below). */
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

/*
 * A cause worth naming for the errors these functions return on their own,
 * and ALSA's description for the rest.
 */
const char *ua1000_midi_strerror(int err)
{
	switch (err) {
	case -ENODEV:
		return "no UA-1000 found (is it connected and powered on?)";
	case -ENXIO:
		return "the UA-1000's driver exposes no control port: it needs "
		       "the cable 1 fix (a driver patched to cable 1 alone "
		       "can be named with --port)";
	case -ETIMEDOUT:
		return "the UA-1000 stopped taking messages (no room freed "
		       "in " STRINGIFY(
			       UA1000_MIDI_WRITE_TIMEOUT_MS) " ms); it may be "
							     "busy switching "
							     "its clock, or "
							     "have lost its "
							     "clock "
							     "source";
	case -EBUSY:
		return "the MIDI port is in use by another program "
		       "(is ua1000gui connected?)";
	default:
		return snd_strerror(err);
	}
}
