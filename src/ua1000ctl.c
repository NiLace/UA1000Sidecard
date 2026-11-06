/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000ctl - command-line control of the Roland/EDIROL UA-1000
 *
 * Each command changes the editor's state (shared with ua1000gui), sends the
 * resulting messages to the device and saves the state.  See ua1000ctl(1).
 *
 * One run is one command: main() loads the state file, a cmd_*() handler
 * parses the command's arguments and applies them through the
 * ua1000_edit_*() functions of ua1000_state.c, collecting the messages in
 * struct ctl; then the messages are sent (or printed, with --dry-run) and
 * the state is saved only when an edit changed it.  Nothing reaches the
 * device or the state file when the command line is refused, so a typo
 * never leaves the two out of step.
 *
 * This file builds no SysEx of its own: every message comes from
 * ua1000_proto.c or ua1000_state.c, and the port from ua1000_midi.c.  It
 * does not stay running: the device's messages are read only right after
 * a memory load (the dump that follows it, then the device's state) and
 * for the read command.
 *
 * Exit codes: 0, EXIT_RUNTIME (the device or the state file failed) or
 * EXIT_USAGE (the command line was not understood).
 */
#include "ua1000_midi.h"
#include "ua1000_state.h"
#include "ua1000_statefile.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define VERSION "0.2"

#define EXIT_RUNTIME 1
#define EXIT_USAGE 2

/* Room for the messages of one command (checked against a sync below). */
#define OUT_MAX 128

/* One run: the state, the messages to send and what to do afterwards. */
struct ctl {
	struct ua1000_state state;
	size_t count; /* messages queued in out[] */
	struct ua1000_msg out[OUT_MAX];
	bool state_changed; /* the state file must be written */
	/* after a memory load: read the device's state (it holds what the
	 * memory loaded) once its dump has come */
	bool read_after;
	/* Wait for the dump the device sends when a load changes its panel. */
	bool listen;
};

/*
 * Print the help text to f: stdout for --help, stderr when no command was
 * given.  Keep it in step with ua1000ctl(1).
 */
static void usage(FILE *f)
{
	fputs("Usage: ua1000ctl [OPTION]... COMMAND [ARGUMENT]...\n"
	      "Control the mixer, patch bay and panel switches of a UA-1000.\n"
	      "\n"
	      "Options:\n"
	      "  -p, --port PORT    ALSA rawmidi port (default: found by "
	      "name)\n"
	      "  -s, --state FILE   state file (default: "
	      "$XDG_STATE_HOME/ua1000/state)\n"
	      "  -n, --dry-run      print the messages instead of sending "
	      "them\n"
	      "  -h, --help         show this help\n"
	      "  -V, --version      show the version\n"
	      "\n"
	      "Commands:\n"
	      "  status                          print the known state\n"
	      "  read                            print the device's own "
	      "state, read from it\n"
	      "  ports                           list the MIDI ports\n"
	      "  sync                            send the whole known state\n"
	      "  input|output CH gain DB         DB: -inf, or any level up to "
	      "+12\n"
	      "  input|output CH pan POS         POS: L20..L1, C, R1..R20\n"
	      "  input|output CH mute|solo on|off\n"
	      "  input|output PAIR link on|off   PAIR: 1-2, 3-4, 5-6, 7-8, "
	      "9-10\n"
	      "  monitor gain DB [left|right]\n"
	      "  monitor link on|off\n"
	      "  panel mono|soft-ctrl on|off\n"
	      "  panel output-volume main|all\n"
	      "  panel clock internal|ext-spdif|ext-adat\n"
	      "  panel digital-out spdif|adat\n"
	      "  route wave-in|output-dm-on|output-dm-off PAIR SOURCE\n"
	      "  preset wave-in initial|loop-back\n"
	      "  preset output initial|hard-thru|monitor-out\n"
	      "  memory store|load SLOT [SECTION,...]\n"
	      "                                  SLOT: 1-5; SECTION: panel, "
	      "wave-in,\n"
	      "                                  output, monitor (default: "
	      "all)\n"
	      "  user-set RATE                   power-on settings, at RATE "
	      "Hz\n"
	      "\n"
	      "SOURCE: monitor-out, input-1-2 .. input-9-10,\n"
	      "        wave-out-1-2 .. wave-out-9-10, and for output-dm-off "
	      "also same.\n",
	      f);
}

#ifdef __GNUC__
static int bad(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

/*
 * Report a command line that is not understood: the message, prefixed with
 * the program's name, and a pointer to --help, on stderr.  Returns
 * EXIT_USAGE so that a caller can write "return bad(...)".
 */
static int bad(const char *fmt, ...)
{
	va_list ap;

	fputs("ua1000ctl: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputs("\nTry 'ua1000ctl --help'.\n", stderr);
	return EXIT_USAGE;
}

/* The largest command is a sync. */
_Static_assert(OUT_MAX >= sizeof(((struct ua1000_sync *)0)->msg) /
				  sizeof(struct ua1000_msg),
	       "OUT_MAX must hold a whole sync");

/*
 * Queue count messages for sending.  No bounds check: a run is one command,
 * and the largest one (a sync) fits, as the assertion above proves.
 */
static void add(struct ctl *ctl, const struct ua1000_msg *msg, size_t count)
{
	for (size_t i = 0; i < count; i++)
		ctl->out[ctl->count++] = msg[i];
}

/*
 * Queue an edit's messages and mark the state for saving.  Used by every
 * handler whose command changes the state; user-set and sync call add().
 */
static void add_edit(struct ctl *ctl, const struct ua1000_edit *edit)
{
	add(ctl, edit->msg, edit->count);
	ctl->state_changed = true;
}

/*
 * ---- Argument parsing ----
 *
 * Each parser returns false for a NULL, malformed or out-of-range word; the
 * handler turns that into bad().
 */

/* A plain decimal number in min..max into *value. */
static bool parse_uint(const char *s, unsigned int min, unsigned int max,
		       unsigned int *value)
{
	char *end;
	unsigned long v;

	/* plain decimal: no sign, no leading zero ("05"), no "0x" */
	if (!s || s[0] < '0' || s[0] > '9' || (s[0] == '0' && s[1]))
		return false;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || *end || v < min || v > max)
		return false;
	*value = (unsigned int)v;
	return true;
}

/* One of two words: yes sets *value to true, no to false. */
static bool parse_choice(const char *s, const char *yes, const char *no,
			 bool *value)
{
	if (s && strcmp(s, yes) == 0)
		*value = true;
	else if (s && strcmp(s, no) == 0)
		*value = false;
	else
		return false;
	return true;
}

/* "on" or "off", for the switches of every command. */
static bool parse_on_off(const char *s, bool *value)
{
	return parse_choice(s, "on", "off", value);
}

/* A channel 1..10, returned as 0..9. */
static bool parse_channel(const char *s, unsigned int *channel)
{
	if (!parse_uint(s, 1, UA1000_CHANNELS, channel))
		return false;
	(*channel)--;
	return true;
}

/* A pair "1-2" .. "9-10", returned as 0..4. */
static bool parse_pair(const char *s, unsigned int *pair)
{
	for (unsigned int p = 0; p < UA1000_PAIRS; p++) {
		char name[8];

		snprintf(name, sizeof(name), "%u-%u", 2 * p + 1, 2 * p + 2);
		if (s && strcmp(s, name) == 0) {
			*pair = p;
			return true;
		}
	}
	return false;
}

/*
 * Plain decimal decibels: an optional sign, digits, an optional fraction
 * ("-6", "+1.5", "0.25").  strtod alone would also take "0x1", "1e1",
 * "inf" and "nan".
 */
static bool plain_decimal(const char *s, const char **end)
{
	bool digits = false;

	if (*s == '+' || *s == '-')
		s++;
	for (; *s >= '0' && *s <= '9'; s++)
		digits = true;
	if (*s == '.')
		for (s++; *s >= '0' && *s <= '9'; s++)
			digits = true;
	*end = s;
	return digits;
}

/*
 * "-inf", or decibels with an optional "dB" (or "db"), into a gain value.
 * Above the device's maximum (+12 dB, with a rounding margin) is refused;
 * there is no lower limit: ua1000_gain_from_db() rounds a very low level
 * down to silence.
 */
static bool parse_db(const char *s, uint32_t *gain)
{
	const char *number_end;
	char *end;
	double db;

	if (!s)
		return false;
	if (strcasecmp(s, "-inf") == 0) {
		*gain = 0;
		return true;
	}
	if (!plain_decimal(s, &number_end))
		return false;
	errno = 0;
	db = strtod(s, &end);
	if (end != number_end || errno || !isfinite(db))
		return false;
	if (*end && strcmp(end, "dB") != 0 && strcmp(end, "db") != 0)
		return false;
	if (db > ua1000_gain_to_db(UA1000_GAIN_MAX) + 0.005)
		return false;
	*gain = ua1000_gain_from_db(db);
	return true;
}

/* "C", "L1".."L20", "R1".."R20", in either case. */
static bool parse_pan(const char *s, uint16_t *pan)
{
	unsigned int steps;
	char side;

	if (!s)
		return false;
	side = (char)toupper((unsigned char)s[0]);
	if (side == 'C' && !s[1]) {
		*pan = ua1000_pan_from_position(0);
		return true;
	}
	if ((side != 'L' && side != 'R') ||
	    !parse_uint(s + 1, 1, UA1000_PAN_STEPS, &steps))
		return false;
	*pan = ua1000_pan_from_position(side == 'L' ? -(int)steps : (int)steps);
	return true;
}

/*
 * A memory command's sections: "all" or NULL (the argument left out) for
 * every section, else a comma-separated list of the names below, in the
 * order of the bits of enum ua1000_section.  An empty list is refused, as
 * is a list longer than the local copy strtok_r() needs.
 */
static bool parse_sections(const char *s, unsigned int *sections)
{
	static const char *const names[] = { "panel", "wave-in", "output",
					     "monitor" };
	char buf[64];
	char *save = NULL;

	if (!s || strcmp(s, "all") == 0) {
		*sections = UA1000_SECTION_ALL;
		return true;
	}
	if (snprintf(buf, sizeof(buf), "%s", s) >= (int)sizeof(buf))
		return false;
	*sections = 0;
	for (const char *w = strtok_r(buf, ",", &save); w;
	     w = strtok_r(NULL, ",", &save)) {
		unsigned int i = 0;

		while (i < 4 && strcmp(w, names[i]) != 0)
			i++;
		if (i == 4)
			return false;
		*sections |= 1u << i;
	}
	return *sections != 0;
}

/*
 * ---- Commands ----
 *
 * Each handler gets the command's words (argv[0] is the command's name),
 * queues its messages in ctl and returns 0, or bad()'s EXIT_USAGE with
 * nothing queued.  None of them touches the device: main() sends.
 */

/*
 * input|output CH gain|pan|mute|solo VALUE, and input|output PAIR link
 * on|off, for the mixer given.  Called by cmd_input() and cmd_output().
 */
static int cmd_strip(struct ctl *ctl, enum ua1000_mixer mixer, int argc,
		     char **argv)
{
	struct ua1000_edit edit;
	unsigned int channel, pair;
	bool on;

	if (argc != 4)
		return bad("%s needs a channel, a parameter and a value",
			   argv[0]);

	const char *param = argv[2], *value = argv[3];

	/* link names a pair, every other parameter a channel */
	if (strcmp(param, "link") == 0) {
		if (!parse_pair(argv[1], &pair))
			return bad("'%s' is not a pair (1-2 .. 9-10)", argv[1]);
		if (!parse_on_off(value, &on))
			return bad("link takes on or off, not '%s'", value);
		ua1000_edit_link(&ctl->state, &edit, mixer, pair, on);
		add_edit(ctl, &edit);
		return 0;
	}
	if (!parse_channel(argv[1], &channel))
		return bad("'%s' is not a channel (1 .. 10)", argv[1]);

	if (strcmp(param, "gain") == 0) {
		uint32_t gain;

		if (!parse_db(value, &gain))
			return bad("'%s' is not a gain (-inf, or dB up to +12)",
				   value);
		ua1000_edit_gain(&ctl->state, &edit, mixer, channel, gain);
	} else if (strcmp(param, "pan") == 0) {
		uint16_t pan;

		if (!parse_pan(value, &pan))
			return bad("'%s' is not a pan position (L20..C..R20)",
				   value);
		ua1000_edit_pan(&ctl->state, &edit, mixer, channel, pan);
	} else if (strcmp(param, "mute") == 0 || strcmp(param, "solo") == 0) {
		if (!parse_on_off(value, &on))
			return bad("expected on or off, not '%s'", value);
		if (param[0] == 'm')
			ua1000_edit_mute(&ctl->state, &edit, mixer, channel,
					 on);
		else
			ua1000_edit_solo(&ctl->state, &edit, mixer, channel,
					 on);
	} else {
		return bad("unknown channel parameter '%s'", param);
	}
	add_edit(ctl, &edit);
	return 0;
}

/* The input command: a strip of the INPUT mixer. */
static int cmd_input(struct ctl *ctl, int argc, char **argv)
{
	return cmd_strip(ctl, UA1000_MIXER_INPUT, argc, argv);
}

/* The output command: a strip of the OUTPUT mixer. */
static int cmd_output(struct ctl *ctl, int argc, char **argv)
{
	return cmd_strip(ctl, UA1000_MIXER_OUTPUT, argc, argv);
}

/*
 * monitor gain DB [left|right], or monitor link on|off.  Without a side the
 * gain goes to both; with one while the monitor is linked, a warning says
 * the other side follows too.
 */
static int cmd_monitor(struct ctl *ctl, int argc, char **argv)
{
	struct ua1000_edit edit;

	if (argc == 3 && strcmp(argv[1], "link") == 0) {
		bool on;

		if (!parse_on_off(argv[2], &on))
			return bad("link takes on or off, not '%s'", argv[2]);
		ua1000_edit_monitor_link(&ctl->state, &edit, on);
	} else if ((argc == 3 || argc == 4) && strcmp(argv[1], "gain") == 0) {
		uint32_t gain;

		if (!parse_db(argv[2], &gain))
			return bad("'%s' is not a gain (-inf, or dB up to +12)",
				   argv[2]);
		if (argc == 3) {
			/* Both sides; a linked monitor does that by itself. */
			ua1000_edit_monitor_gain(&ctl->state, &edit,
						 UA1000_LEFT, gain);
			if (!ctl->state.monitor_link) {
				add_edit(ctl, &edit);
				ua1000_edit_monitor_gain(&ctl->state, &edit,
							 UA1000_RIGHT, gain);
			}
		} else {
			enum ua1000_side side;

			if (strcmp(argv[3], "left") == 0)
				side = UA1000_LEFT;
			else if (strcmp(argv[3], "right") == 0)
				side = UA1000_RIGHT;
			else
				return bad("expected left or right, not '%s'",
					   argv[3]);
			if (ctl->state.monitor_link)
				fputs("ua1000ctl: the monitor is linked: both "
				      "sides follow\n",
				      stderr);
			ua1000_edit_monitor_gain(&ctl->state, &edit, side,
						 gain);
		}
	} else {
		return bad("%s takes 'gain DB [left|right]' or 'link on|off'",
			   argv[0]);
	}
	add_edit(ctl, &edit);
	return 0;
}

/*
 * panel SWITCH VALUE: one switch changed in a copy of the panel, which is
 * then written whole (the switches travel as one block).  An ADAT setting
 * at a rate the device ignores it at is refused with EXIT_USAGE.
 */
static int cmd_panel(struct ctl *ctl, int argc, char **argv)
{
	struct ua1000_panel panel = ctl->state.panel;
	struct ua1000_edit edit;

	if (argc != 3)
		return bad("%s needs a switch and a value", argv[0]);

	const char *sw = argv[1], *value = argv[2];
	bool ok;

	if (strcmp(sw, "mono") == 0)
		ok = parse_on_off(value, &panel.mono);
	else if (strcmp(sw, "soft-ctrl") == 0)
		ok = parse_on_off(value, &panel.soft_ctrl);
	else if (strcmp(sw, "output-volume") == 0)
		ok = parse_choice(value, "all", "main", &panel.output_all);
	else if (strcmp(sw, "digital-out") == 0)
		ok = parse_choice(value, "adat", "spdif",
				  &panel.digital_out_adat);
	else if (strcmp(sw, "clock") == 0)
		ok = ua1000_clock_parse(value, &panel.clock);
	else
		return bad("unknown panel switch '%s'", sw);
	if (!ok)
		return bad("'%s' is not a value of this switch", value);

	/* The vendor's panel selects ADAT output with the ADAT clock. */
	if (strcmp(sw, "clock") == 0 && panel.clock == UA1000_CLOCK_EXT_ADAT)
		panel.digital_out_adat = true;
	if (ua1000_edit_panel(&ctl->state, &edit, &panel) == -ERANGE)
		return bad("ADAT works only at 44.1 or 48 kHz; the device runs "
			   "at %.1f kHz (Owner's Manual, p. 41)",
			   ua1000_rate_hz(panel.rate) / 1000.0);
	add_edit(ctl, &edit);
	return 0;
}

/*
 * route TABLE PAIR SOURCE: one pair of one patch-bay table.  A source the
 * table does not offer (same outside output-dm-off) is refused by
 * ua1000_edit_route().
 */
static int cmd_route(struct ctl *ctl, int argc, char **argv)
{
	enum ua1000_route_table table;
	enum ua1000_source source;
	struct ua1000_edit edit;
	unsigned int pair;

	if (argc != 4)
		return bad("%s needs a table, a pair and a source", argv[0]);
	if (!ua1000_route_table_parse(argv[1], &table))
		return bad("unknown routing table '%s'", argv[1]);
	if (!parse_pair(argv[2], &pair))
		return bad("'%s' is not a pair (1-2 .. 9-10)", argv[2]);
	if (!ua1000_source_parse(argv[3], &source) ||
	    ua1000_edit_route(&ctl->state, &edit, table, pair, source) != 0)
		return bad("'%s' is not a source of this table", argv[3]);
	add_edit(ctl, &edit);
	return 0;
}

/*
 * preset wave-in|output NAME: one of the vendor's patch-bay presets; the
 * other table is kept as it is.
 */
static int cmd_preset(struct ctl *ctl, int argc, char **argv)
{
	enum ua1000_wave_in_preset wave_in = UA1000_WAVE_IN_KEEP;
	enum ua1000_output_preset output = UA1000_OUTPUT_KEEP;
	struct ua1000_edit edit;

	if (argc != 3)
		return bad("%s needs a table and a preset", argv[0]);

	const char *table = argv[1], *name = argv[2];

	if (strcmp(table, "wave-in") == 0 && strcmp(name, "initial") == 0)
		wave_in = UA1000_WAVE_IN_INITIAL;
	else if (strcmp(table, "wave-in") == 0 &&
		 strcmp(name, "loop-back") == 0)
		wave_in = UA1000_WAVE_IN_LOOP_BACK;
	else if (strcmp(table, "output") == 0 && strcmp(name, "initial") == 0)
		output = UA1000_OUTPUT_INITIAL;
	else if (strcmp(table, "output") == 0 && strcmp(name, "hard-thru") == 0)
		output = UA1000_OUTPUT_HARD_THRU;
	else if (strcmp(table, "output") == 0 &&
		 strcmp(name, "monitor-out") == 0)
		output = UA1000_OUTPUT_MONITOR_OUT;
	else
		return bad("unknown preset '%s'", name);
	ua1000_edit_preset(&ctl->state, &edit, wave_in, output);
	add_edit(ctl, &edit);
	return 0;
}

/*
 * memory store|load SLOT [SECTIONS].  A load also asks main() to read the
 * device afterwards (read_after), and to wait first for the dump it sends
 * when the load touches the panel or the patch bay (listen).
 */
static int cmd_memory(struct ctl *ctl, int argc, char **argv)
{
	struct ua1000_edit edit;
	unsigned int slot, sections;
	bool known;

	if (argc != 3 && argc != 4)
		return bad("%s needs store or load, a slot and sections",
			   argv[0]);
	if (!parse_uint(argv[2], 1, UA1000_MEMORY_SLOTS, &slot))
		return bad("'%s' is not a memory slot (1 .. 5)", argv[2]);
	if (!parse_sections(argc == 4 ? argv[3] : NULL, &sections))
		return bad("'%s' is not a list of sections", argv[argc - 1]);
	if (strcmp(argv[1], "store") == 0) {
		ua1000_edit_store(&ctl->state, &edit, slot, sections);
	} else if (strcmp(argv[1], "load") == 0) {
		ua1000_edit_load(&ctl->state, &edit, slot, sections, &known);
		ctl->read_after = true;
		ctl->listen = sections &
			      (UA1000_SECTION_PANEL | UA1000_SECTION_WAVE_IN |
			       UA1000_SECTION_OUTPUT);
		(void)known; /* the device is read after the load */
	} else {
		return bad("expected store or load, not '%s'", argv[1]);
	}
	add_edit(ctl, &edit);
	return 0;
}

/*
 * user-set RATE: store the power-on settings with the panel as it is, at
 * RATE.  The state does not change (the device runs at its rate until it
 * is switched on again), so nothing is saved.
 */
static int cmd_user_set(struct ctl *ctl, int argc, char **argv)
{
	struct ua1000_panel panel = ctl->state.panel;
	struct ua1000_msg msg;
	unsigned int hz;

	if (argc != 2 || !parse_uint(argv[1], 1, 1000000, &hz) ||
	    !ua1000_rate_from_hz(hz, &panel.rate))
		return bad("%s takes 44100, 48000, 88200 or 96000", argv[0]);
	if ((panel.digital_out_adat || panel.clock == UA1000_CLOCK_EXT_ADAT) &&
	    !ua1000_rate_allows_adat(panel.rate))
		return bad("ADAT works only at 44.1 or 48 kHz: turn ADAT off "
			   "before storing a USER SET at %u Hz "
			   "(Owner's Manual, p. 41)",
			   hz);
	ua1000_build_user_set(&msg, &panel);
	add(ctl, &msg, 1);
	return 0;
}

/*
 * sync: send the whole state, which also makes a mixer left unknown by a
 * foreign memory load known again.
 */
static int cmd_sync(struct ctl *ctl, int argc, char **argv)
{
	struct ua1000_sync sync;

	if (argc != 1)
		return bad("%s takes no arguments", argv[0]);
	ua1000_state_sync(&ctl->state, &sync);
	add(ctl, sync.msg, sync.count);
	/* the device now has the mixer this state shows */
	if (ctl->state.mixer_unknown) {
		ctl->state.mixer_unknown = false;
		ctl->state_changed = true;
	}
	return 0;
}

/* A command that edits the state; main() runs it by name. */
struct command {
	const char *name;
	int (*run)(struct ctl *ctl, int argc, char **argv);
};

/* status, read and ports are not here: main() handles them before. */
static const struct command commands[] = {
	{ "input", cmd_input },	    { "output", cmd_output },
	{ "monitor", cmd_monitor }, { "panel", cmd_panel },
	{ "route", cmd_route },	    { "preset", cmd_preset },
	{ "memory", cmd_memory },   { "user-set", cmd_user_set },
	{ "sync", cmd_sync },
};

/* ---- Main ---- */

/* --dry-run: each queued message on its own line, in hex bytes. */
static void print_messages(const struct ctl *ctl)
{
	for (size_t i = 0; i < ctl->count; i++) {
		for (size_t j = 0; j < ctl->out[i].len; j++)
			printf("%s%02X", j ? " " : "", ctl->out[i].bytes[j]);
		putchar('\n');
	}
}

/*
 * ua1000_midi_receive() handler for read_dump(): apply what the device
 * reported (its panel or patch bay after a load) to the state.  Messages
 * that decode to nothing known are ignored.
 */
static void on_device_message(const struct ua1000_dt1 *dt1, void *data)
{
	struct ctl *ctl = data;
	struct ua1000_event event = { 0 };

	/* the decoder fills in only what the message carries: start it from
	 * the state, so the rest of the panel and patch bay stays as is */
	event.panel = ctl->state.panel;
	event.patchbay = ctl->state.patchbay;
	ua1000_decode(dt1, &event);
	if (event.type == UA1000_EVENT_UNKNOWN)
		return;
	ua1000_state_apply_event(&ctl->state, &event);
	ctl->state_changed = true;
}

/*
 * The device dumps its panel block after a load that changes the panel or
 * the patch bay, and sends nothing when the load changes neither: read
 * whatever arrives within DUMP_WAIT_MS.
 */
#define DUMP_WAIT_MS 1000

/*
 * Apply whatever the device sends until it has been quiet for DUMP_WAIT_MS.
 * Called by send_messages() after a load (ctl->listen).  Returns 0, or the
 * negative errno of a failed port.
 */
static int read_dump(struct ctl *ctl, struct ua1000_midi *midi)
{
	struct pollfd fds[4];
	int n = ua1000_midi_poll_descriptors(midi, fds, 4);

	while (n > 0 && poll(fds, (nfds_t)n, DUMP_WAIT_MS) > 0) {
		int err = ua1000_midi_receive(midi, on_device_message, ctl);

		if (err != 0)
			return err;
	}
	return 0;
}

/* ---- read ---- */

/*
 * The answer to a block request, kept past the receive call: dt1.data
 * points into data, since the receive buffer is reused.
 */
struct block_wait {
	uint8_t data[UA1000_GLOBAL_BLOCK_SIZE];
	struct ua1000_dt1 dt1;
	bool got;
};

/*
 * ua1000_midi_receive() handler for read_device() and cmd_read(): keep the
 * first message that is the whole current block, ignore everything else.
 */
static void on_block(const struct ua1000_dt1 *dt1, void *data)
{
	struct block_wait *w = data;

	if (w->got || dt1->addr[0] != 0x02 ||
	    dt1->addr[1] != UA1000_BLOCK_CURRENT ||
	    dt1->len != UA1000_GLOBAL_BLOCK_SIZE)
		return;
	memcpy(w->data, dt1->data, sizeof(w->data));
	w->dt1 = *dt1;
	w->dt1.data = w->data;
	w->got = true;
}

/*
 * After a memory load: ask the device for its state, which now holds what
 * the memory loaded, and take it (keeping the solos and the memories this
 * editor stored, and giving the device the solos).  Without an answer the
 * state keeps what it had, and says so if the mixer is not known.
 * Called by send_messages() when ctl->read_after is set.  Returns 0 (with
 * or without an answer), or the negative errno of a failed port.
 */
static int read_device(struct ctl *ctl, struct ua1000_midi *midi)
{
	static struct block_wait wait;
	struct ua1000_msg request;
	struct ua1000_state device;
	struct ua1000_sync solos;
	struct pollfd fds[4];
	int n, err;

	wait.got = false;
	ua1000_build_block_request(&request, UA1000_BLOCK_CURRENT);
	err = ua1000_midi_send(midi, &request, 1);
	if (err == 0)
		err = ua1000_midi_drain(midi);
	n = ua1000_midi_poll_descriptors(midi, fds, 4);
	while (err == 0 && !wait.got && n > 0 &&
	       poll(fds, (nfds_t)n, DUMP_WAIT_MS) > 0)
		err = ua1000_midi_receive(midi, on_block, &wait);
	if (err != 0)
		return err;
	if (!wait.got || ua1000_state_from_block(&device, &wait.dt1) != 0) {
		if (ctl->state.mixer_unknown)
			fputs("ua1000ctl: the loaded memory was not stored by "
			      "this editor, and the device did not report "
			      "it: its mixer settings are unknown\n",
			      stderr);
		return 0;
	}
	ua1000_state_adopt_device(&ctl->state, &device, &solos);
	ctl->state_changed = true;
	err = ua1000_midi_send(midi, solos.msg, solos.count);
	return err == 0 ? ua1000_midi_drain(midi) : err;
}

/*
 * Open the port (the one given, or the device's, found by name), send the
 * queued messages and wait for them to leave; after a memory load, also
 * take the dump and the device's state.  The port is closed before
 * returning, so another program can open it.  Errors are reported here;
 * returns 0 or EXIT_RUNTIME.
 */
static int send_messages(struct ctl *ctl, const char *port)
{
	char found[32];
	struct ua1000_midi *midi;
	int err;

	if (!port) {
		err = ua1000_midi_find(found, sizeof(found));
		if (err != 0) {
			fprintf(stderr, "ua1000ctl: %s\n",
				ua1000_midi_strerror(err));
			return EXIT_RUNTIME;
		}
		port = found;
	}
	err = ua1000_midi_open(&midi, port, ctl->listen || ctl->read_after);
	if (err != 0) {
		fprintf(stderr, "ua1000ctl: %s: %s\n", port,
			ua1000_midi_strerror(err));
		return EXIT_RUNTIME;
	}
	err = ua1000_midi_send(midi, ctl->out, ctl->count);
	if (err == 0)
		err = ua1000_midi_drain(midi);
	if (err == 0 && ctl->listen)
		err = read_dump(ctl, midi);
	if (err == 0 && ctl->read_after)
		err = read_device(ctl, midi);
	ua1000_midi_close(midi);
	if (err != 0) {
		fprintf(stderr, "ua1000ctl: %s: %s\n", port,
			ua1000_midi_strerror(err));
		return EXIT_RUNTIME;
	}
	return 0;
}

/*
 * The read command: ask the device for its state block and print what it
 * holds, in the state file's format, on stdout.  The state file is not
 * changed.  With dry_run, print the request instead.  Returns 0,
 * or EXIT_RUNTIME when the port fails or the device gives no usable answer.
 */
static int cmd_read(const char *port, bool dry_run)
{
	static struct block_wait wait;
	struct ua1000_msg request;
	struct ua1000_state state;
	struct ua1000_midi *midi;
	struct pollfd fds[4];
	char found[32];
	int n, err;

	ua1000_build_block_request(&request, UA1000_BLOCK_CURRENT);
	if (dry_run) {
		for (size_t j = 0; j < request.len; j++)
			printf("%s%02X", j ? " " : "", request.bytes[j]);
		putchar('\n');
		return 0;
	}
	if (!port) {
		err = ua1000_midi_find(found, sizeof(found));
		if (err != 0) {
			fprintf(stderr, "ua1000ctl: %s\n",
				ua1000_midi_strerror(err));
			return EXIT_RUNTIME;
		}
		port = found;
	}
	err = ua1000_midi_open(&midi, port, true);
	if (err != 0) {
		fprintf(stderr, "ua1000ctl: %s: %s\n", port,
			ua1000_midi_strerror(err));
		return EXIT_RUNTIME;
	}
	err = ua1000_midi_send(midi, &request, 1);
	if (err == 0)
		err = ua1000_midi_drain(midi);
	n = ua1000_midi_poll_descriptors(midi, fds, 4);
	while (err == 0 && !wait.got && n > 0 &&
	       poll(fds, (nfds_t)n, DUMP_WAIT_MS) > 0)
		err = ua1000_midi_receive(midi, on_block, &wait);
	ua1000_midi_close(midi);
	if (err != 0) {
		fprintf(stderr, "ua1000ctl: %s: %s\n", port,
			ua1000_midi_strerror(err));
		return EXIT_RUNTIME;
	}
	if (!wait.got) {
		fprintf(stderr, "ua1000ctl: %s: the device did not answer\n",
			port);
		return EXIT_RUNTIME;
	}
	if (ua1000_state_from_block(&state, &wait.dt1) != 0) {
		fprintf(stderr,
			"ua1000ctl: %s: the device's answer could not "
			"be read\n",
			port);
		return EXIT_RUNTIME;
	}
	/* the running rate is the driver's: the block's byte is not a rate
	 * code at a fixed SAMPLE RATE position */
	{
		unsigned int hz;

		if (ua1000_midi_card_rate(&hz) == 0)
			ua1000_rate_from_hz(hz, &state.panel.rate);
	}
	puts("# read from the device; the solos are not read (shown off)");
	ua1000_state_write(&state, stdout);
	return 0;
}

/*
 * Options, then the command.  ports, read and status need no edit; every
 * other command goes through the commands[] table, then send_messages(),
 * then the state file.  Options end at the first word without a leading
 * '-', so an argument such as -inf or -6 after the command is not taken
 * for one.
 */
int main(int argc, char **argv)
{
	static struct ctl ctl;
	const char *port = NULL, *state_path = NULL;
	char default_path[4096], err[256];
	bool dry_run = false, existed;
	int i;

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		const char *opt = argv[i];

		if (strcmp(opt, "-h") == 0 || strcmp(opt, "--help") == 0) {
			usage(stdout);
			return 0;
		}
		if (strcmp(opt, "-V") == 0 || strcmp(opt, "--version") == 0) {
			puts("ua1000ctl " VERSION);
			return 0;
		}
		if (strcmp(opt, "-n") == 0 || strcmp(opt, "--dry-run") == 0) {
			dry_run = true;
		} else if ((strcmp(opt, "-p") == 0 ||
			    strcmp(opt, "--port") == 0) &&
			   i + 1 < argc) {
			port = argv[++i];
		} else if ((strcmp(opt, "-s") == 0 ||
			    strcmp(opt, "--state") == 0) &&
			   i + 1 < argc) {
			state_path = argv[++i];
		} else {
			return bad("unknown or incomplete option '%s'", opt);
		}
	}
	if (i == argc) {
		usage(stderr);
		return EXIT_USAGE;
	}

	const char *name = argv[i];

	/* listing the ports needs no state file */
	if (strcmp(name, "ports") == 0) {
		ua1000_midi_list(stdout);
		return 0;
	}

	if (!state_path) {
		int ret = ua1000_statefile_default_path(default_path,
							sizeof(default_path));

		if (ret != 0) {
			fprintf(stderr, "ua1000ctl: no state file: %s\n",
				ret == -ENOENT ? "neither XDG_STATE_HOME nor "
						 "HOME is set"
					       : strerror(-ret));
			return EXIT_RUNTIME;
		}
		state_path = default_path;
	}
	if (ua1000_statefile_load(state_path, &ctl.state, &existed, err,
				  sizeof(err)) != 0) {
		fprintf(stderr, "ua1000ctl: %s\n", err);
		return EXIT_RUNTIME;
	}

	if (strcmp(name, "read") == 0) {
		if (argc - i != 1)
			return bad("read takes no arguments");
		return cmd_read(port, dry_run);
	}
	if (strcmp(name, "status") == 0) {
		if (!existed)
			fputs("# no state saved yet: these are the defaults\n",
			      stdout);
		ua1000_state_write(&ctl.state, stdout);
		return 0;
	}

	/*
	 * The panel block carries the sample rate: give it the one the device
	 * runs at, which the driver reports, rather than the saved one.
	 */
	unsigned int hz;

	if (!dry_run && ua1000_midi_card_rate(&hz) == 0)
		ua1000_rate_from_hz(hz, &ctl.state.panel.rate);

	const struct command *cmd = NULL;

	for (size_t c = 0; c < sizeof(commands) / sizeof(commands[0]); c++)
		if (strcmp(name, commands[c].name) == 0)
			cmd = &commands[c];
	if (!cmd)
		return bad("unknown command '%s'", name);

	int ret = cmd->run(&ctl, argc - i, argv + i);

	if (ret != 0)
		return ret;
	if (dry_run) {
		print_messages(&ctl);
		return 0;
	}
	ret = send_messages(&ctl, port);
	if (ret != 0)
		return ret;
	/* saved only after a successful send: when the send fails, the file
	 * keeps the state as it was before this command */
	if (ctl.state_changed) {
		int err_save = ua1000_statefile_save(state_path, &ctl.state);

		if (err_save != 0) {
			fprintf(stderr, "ua1000ctl: %s: %s\n", state_path,
				strerror(-err_save));
			return EXIT_RUNTIME;
		}
	}
	return 0;
}
