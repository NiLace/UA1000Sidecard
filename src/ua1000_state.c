/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000_state.c - what the editor knows about the device, and how to change it
 *
 * This module owns struct ua1000_state, the editor's copy of the device's
 * settings: panel switches, the three patch-bay tables, both MONITOR mixers,
 * MONITOR OUT and what this editor stored in the five memory slots.  It
 * provides:
 *
 *  - the edits (ua1000_edit_*()), which change the state and return the
 *    SysEx messages that make the device follow, the linked partner
 *    included;
 *  - ua1000_state_sync(), every message that brings the device to the state;
 *  - the reading of the device's current-state block, and the adoption of
 *    it as the editor's state;
 *  - a description of the differences between two states, in words;
 *  - the state file's text format (writing and parsing) and the names it
 *    shares with the command line.
 *
 * Invariants kept on the state: every value is within the device's range
 * (an edit that would break one is refused with the state untouched); a
 * linked pair moves together; an unlinked pair has at most one channel
 * soloed, because the device's solo register holds one channel per pair.
 *
 * It does no I/O on the device and knows no file path: the messages are sent
 * by the caller (ua1000_midi.c), and the state file is placed, read and
 * replaced by ua1000_statefile.c.  Its users are ua1000ctl.c, the GUI
 * (src/gui/) and ua1000_statefile.c.
 */
#include "ua1000_state.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The state file's first line: the magic word and the format version. */
#define STATE_MAGIC "ua1000-state"
#define STATE_VERSION 1

/* ---- Names ---- */

/* One entry of a name table: the spelling used in files and on the command
 * line, and the enum value it stands for. */
struct name {
	const char *text;
	int value;
};

static const struct name source_names[] = {
	{ "same", UA1000_SRC_SAME_AS_DM_ON },
	{ "monitor-out", UA1000_SRC_MONITOR_OUT },
	{ "input-1-2", UA1000_SRC_INPUT_1_2 },
	{ "input-3-4", UA1000_SRC_INPUT_3_4 },
	{ "input-5-6", UA1000_SRC_INPUT_5_6 },
	{ "input-7-8", UA1000_SRC_INPUT_7_8 },
	{ "input-9-10", UA1000_SRC_INPUT_9_10 },
	{ "wave-out-1-2", UA1000_SRC_WAVE_OUT_1_2 },
	{ "wave-out-3-4", UA1000_SRC_WAVE_OUT_3_4 },
	{ "wave-out-5-6", UA1000_SRC_WAVE_OUT_5_6 },
	{ "wave-out-7-8", UA1000_SRC_WAVE_OUT_7_8 },
	{ "wave-out-9-10", UA1000_SRC_WAVE_OUT_9_10 },
};

static const struct name clock_names[] = {
	{ "internal", UA1000_CLOCK_INTERNAL },
	{ "ext-spdif", UA1000_CLOCK_EXT_SPDIF },
	{ "ext-adat", UA1000_CLOCK_EXT_ADAT },
};

static const struct name mixer_names[] = {
	{ "input", UA1000_MIXER_INPUT },
	{ "output", UA1000_MIXER_OUTPUT },
};

static const struct name table_names[] = {
	{ "wave-in", UA1000_ROUTE_WAVE_IN },
	{ "output-dm-on", UA1000_ROUTE_OUTPUT_DM_ON },
	{ "output-dm-off", UA1000_ROUTE_OUTPUT_DM_OFF },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/*
 * The name of value in the table names[0..n), or NULL when the table has
 * none.  The *_name() functions below are thin typed wrappers over it.
 */
static const char *name_of(const struct name *names, size_t n, int value)
{
	for (size_t i = 0; i < n; i++)
		if (names[i].value == value)
			return names[i].text;
	return NULL;
}

/*
 * Look text up in names[0..n), case-sensitively.  On a match store its value
 * in *value and return true; otherwise return false with *value untouched.
 * Used by the *_parse() functions below.
 */
static bool value_of(const struct name *names, size_t n, const char *text,
		     int *value)
{
	for (size_t i = 0; i < n; i++) {
		if (strcmp(names[i].text, text) == 0) {
			*value = names[i].value;
			return true;
		}
	}
	return false;
}

/* The state file's name for a patch-bay source, or NULL if it has none. */
const char *ua1000_source_name(enum ua1000_source source)
{
	return name_of(source_names, COUNT(source_names), (int)source);
}

/* A patch-bay source from its name. */
bool ua1000_source_parse(const char *name, enum ua1000_source *source)
{
	int value;

	if (!value_of(source_names, COUNT(source_names), name, &value))
		return false;
	*source = (enum ua1000_source)value;
	return true;
}

/* The state file's name for a clock source, or NULL if it has none. */
const char *ua1000_clock_name(enum ua1000_clock clock)
{
	return name_of(clock_names, COUNT(clock_names), (int)clock);
}

/* A clock source from its name. */
bool ua1000_clock_parse(const char *name, enum ua1000_clock *clock)
{
	int value;

	if (!value_of(clock_names, COUNT(clock_names), name, &value))
		return false;
	*clock = (enum ua1000_clock)value;
	return true;
}

/* The state file's name for a mixer, or NULL if it has none. */
const char *ua1000_mixer_name(enum ua1000_mixer mixer)
{
	return name_of(mixer_names, COUNT(mixer_names), (int)mixer);
}

/* A mixer from its name. */
bool ua1000_mixer_parse(const char *name, enum ua1000_mixer *mixer)
{
	int value;

	if (!value_of(mixer_names, COUNT(mixer_names), name, &value))
		return false;
	*mixer = (enum ua1000_mixer)value;
	return true;
}

/* The state file's name for a patch-bay table, or NULL if it has none. */
const char *ua1000_route_table_name(enum ua1000_route_table table)
{
	return name_of(table_names, COUNT(table_names), (int)table);
}

/* A patch-bay table from its name. */
bool ua1000_route_table_parse(const char *name, enum ua1000_route_table *table)
{
	int value;

	if (!value_of(table_names, COUNT(table_names), name, &value))
		return false;
	*table = (enum ua1000_route_table)value;
	return true;
}

/* ---- Defaults ---- */

/*
 * A stereo pair: the odd channel hard left, the even one hard right, as the
 * vendor's control panel shows every pair (the UA-1000 Owner's Manual,
 * p. 37, its screenshot), and linked.  Centred, the computer's stereo
 * playback reached the phones in mono (found on the device).
 */
static void stereo_pair(struct ua1000_mixer_state *mixer, unsigned int pair)
{
	struct ua1000_strip *left = &mixer->strip[2 * (size_t)pair];

	left[0].pan = 0;
	left[1].pan = UA1000_PAN_RAW_MAX;
	mixer->link[pair] = true;
}

/* Every channel at unity, on, centred, unlinked. */
static void mixer_init(struct ua1000_mixer_state *mixer)
{
	for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++)
		mixer->strip[ch] = (struct ua1000_strip){
			.gain = UA1000_GAIN_UNITY,
			.pan = UA1000_PAN_RAW_MAX / 2,
		};
	for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
		mixer->link[pair] = false;
}

/* Fill *state with the editor's defaults. */
void ua1000_state_init(struct ua1000_state *state)
{
	memset(state, 0, sizeof(*state));
	ua1000_panel_init(&state->panel);
	ua1000_patchbay_init(&state->patchbay);
	mixer_init(&state->mixer[UA1000_MIXER_INPUT]);
	mixer_init(&state->mixer[UA1000_MIXER_OUTPUT]);
	/* The computer's playback comes as "five stereo channels" (the
	 * Owner's Manual, p. 38): every OUTPUT pair a stereo pair, as MONITOR
	 * OUT.  Of the inputs, only 9-10, the digital input (S/PDIF), is
	 * stereo; 1-8 are single sources (a voice, a guitar), heard in the
	 * centre: panned hard to a side, a guitar on input 2 would sound in
	 * one ear only. */
	for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
		stereo_pair(&state->mixer[UA1000_MIXER_OUTPUT], pair);
	stereo_pair(&state->mixer[UA1000_MIXER_INPUT], UA1000_PAIRS - 1);
	state->monitor_gain[UA1000_LEFT] = UA1000_GAIN_UNITY;
	state->monitor_gain[UA1000_RIGHT] = UA1000_GAIN_UNITY;
	state->monitor_link = true;
}

/* The defaults, keeping the memories and the running sample rate. */
void ua1000_state_reset(struct ua1000_state *state)
{
	struct ua1000_state fresh;

	ua1000_state_init(&fresh);
	memcpy(fresh.memory, state->memory, sizeof(fresh.memory));
	fresh.panel.rate = state->panel.rate;
	*state = fresh;
}

/* ---- Edits ---- */

/* Whether mixer and channel (0-based) name a strip of the device. */
static bool channel_valid(enum ua1000_mixer mixer, unsigned int channel)
{
	return ua1000_mixer_valid(mixer) && channel < UA1000_CHANNELS;
}

/* The other channel of channel's pair: 0 <-> 1, 2 <-> 3, ... */
static unsigned int partner(unsigned int channel)
{
	return channel ^ 1u;
}

/* Whether channel's pair is linked in mixer. */
static bool linked(const struct ua1000_state *state, enum ua1000_mixer mixer,
		   unsigned int channel)
{
	return state->mixer[mixer].link[channel / 2];
}

/*
 * The common refusal of an edit: no message, -EINVAL.  Callers return it
 * before touching the state, as the edit contract requires.
 */
static int refuse(struct ua1000_edit *edit)
{
	edit->count = 0;
	return -EINVAL;
}

/* Set a strip's gain, and its partner's when the pair is linked. */
int ua1000_edit_gain(struct ua1000_state *state, struct ua1000_edit *edit,
		     enum ua1000_mixer mixer, unsigned int channel,
		     uint32_t gain)
{
	if (!channel_valid(mixer, channel) || gain > UA1000_GAIN_MAX)
		return refuse(edit);

	struct ua1000_strip *strip = state->mixer[mixer].strip;

	edit->count = 0;
	strip[channel].gain = gain;
	ua1000_build_gain(&edit->msg[edit->count++], mixer, channel, gain);
	if (linked(state, mixer, channel)) {
		strip[partner(channel)].gain = gain;
		ua1000_build_gain(&edit->msg[edit->count++], mixer,
				  partner(channel), gain);
	}
	return 0;
}

/* Set a strip's pan; a linked partner gets the mirrored position. */
int ua1000_edit_pan(struct ua1000_state *state, struct ua1000_edit *edit,
		    enum ua1000_mixer mixer, unsigned int channel, uint16_t pan)
{
	if (!channel_valid(mixer, channel) || pan > UA1000_PAN_RAW_MAX)
		return refuse(edit);

	struct ua1000_strip *strip = state->mixer[mixer].strip;

	edit->count = 0;
	strip[channel].pan = pan;
	ua1000_build_pan(&edit->msg[edit->count++], mixer, channel, pan);
	if (linked(state, mixer, channel)) {
		uint16_t mirrored = (uint16_t)(UA1000_PAN_RAW_MAX - pan);

		strip[partner(channel)].pan = mirrored;
		ua1000_build_pan(&edit->msg[edit->count++], mixer,
				 partner(channel), mirrored);
	}
	return 0;
}

/* Mute or unmute a strip, and its partner when the pair is linked. */
int ua1000_edit_mute(struct ua1000_state *state, struct ua1000_edit *edit,
		     enum ua1000_mixer mixer, unsigned int channel, bool mute)
{
	if (!channel_valid(mixer, channel))
		return refuse(edit);

	struct ua1000_strip *strip = state->mixer[mixer].strip;

	edit->count = 0;
	strip[channel].mute = mute;
	ua1000_build_mute(&edit->msg[edit->count++], mixer, channel, mute);
	if (linked(state, mixer, channel)) {
		strip[partner(channel)].mute = mute;
		ua1000_build_mute(&edit->msg[edit->count++], mixer,
				  partner(channel), mute);
	}
	return 0;
}

/*
 * Solo is ONE register per pair, holding which channel is soloed: 0x1 the
 * left, 0x2 the right; for a linked pair either soloes the pair, and the
 * vendor's panel sends 0x2.  It is not a bit set: 0x3 silences everything
 * (measured on the device, music on OUTPUT 1-2 panned L/R:
 * unlinked 01 left only, 02 right only, 03 silence; linked 01 and 02 the
 * pair, 03 silence; the vendor's captures hold only 00, 01 and 02).  So an
 * unlinked pair has at most one channel soloed, and 0x3 is never sent.
 */
static unsigned int solo_mask(const struct ua1000_state *state,
			      enum ua1000_mixer mixer, unsigned int pair)
{
	unsigned int first = 2 * pair;
	const struct ua1000_strip *left = &state->mixer[mixer].strip[first];
	const struct ua1000_strip *right = left + 1;

	if (state->mixer[mixer].link[pair])
		return left->solo || right->solo ? 0x2 : 0x0;
	return left->solo ? 0x1 : right->solo ? 0x2 : 0x0;
}

/* The device's rule in the state: in an unlinked pair, the left channel's
 * solo wins (only an old state file or memory can hold both). */
static void one_solo_per_pair(struct ua1000_mixer_state *mixers, size_t n)
{
	for (size_t m = 0; m < n; m++) {
		for (size_t p = 0; p < UA1000_PAIRS; p++) {
			struct ua1000_strip *left = &mixers[m].strip[2 * p];

			if (!mixers[m].link[p] && left->solo)
				left[1].solo = false;
		}
	}
}

/*
 * Solo a strip or take its solo off.  One message always: the pair's solo
 * register, written whole (solo_mask()).
 */
int ua1000_edit_solo(struct ua1000_state *state, struct ua1000_edit *edit,
		     enum ua1000_mixer mixer, unsigned int channel, bool solo)
{
	if (!channel_valid(mixer, channel))
		return refuse(edit);

	struct ua1000_strip *strip = state->mixer[mixer].strip;
	unsigned int pair = channel / 2;

	strip[channel].solo = solo;
	/* linked: the pair together; unlinked: one solo per pair, so a new
	 * solo takes it from the partner */
	if (linked(state, mixer, channel) || solo)
		strip[partner(channel)].solo = linked(state, mixer, channel) &&
					       solo;
	edit->count = 1;
	ua1000_build_solo(&edit->msg[0], mixer, pair,
			  solo_mask(state, mixer, pair));
	return 0;
}

/* Link or unlink a pair: the link message, then the pair's solo register. */
int ua1000_edit_link(struct ua1000_state *state, struct ua1000_edit *edit,
		     enum ua1000_mixer mixer, unsigned int pair, bool link)
{
	if (!ua1000_mixer_valid(mixer) || pair >= UA1000_PAIRS)
		return refuse(edit);

	struct ua1000_strip *left =
		&state->mixer[mixer].strip[2 * (size_t)pair];
	bool soloed = left[0].solo || left[1].solo;

	state->mixer[mixer].link[pair] = link;
	/*
	 * The solo through a link change, so the strips show what is heard:
	 * linking a pair with a channel soloed solos the pair (the device
	 * does: one soloed before the link left the other dark while the pair
	 * was heard); unlinking a soloed pair takes the solo off both, as
	 * the device cannot hold both of an unlinked pair, and keeping one
	 * would be an arbitrary choice.
	 */
	left[0].solo = left[1].solo = link && soloed;
	edit->count = 2;
	ua1000_build_link(&edit->msg[0], mixer, pair, link);
	/* Linking changes how the pair's solo register reads. */
	ua1000_build_solo(&edit->msg[1], mixer, pair,
			  solo_mask(state, mixer, pair));
	return 0;
}

/* Set a MONITOR OUT side's gain, and the other side's when linked. */
int ua1000_edit_monitor_gain(struct ua1000_state *state,
			     struct ua1000_edit *edit, enum ua1000_side side,
			     uint32_t gain)
{
	if ((side != UA1000_LEFT && side != UA1000_RIGHT) ||
	    gain > UA1000_GAIN_MAX)
		return refuse(edit);

	enum ua1000_side other = side == UA1000_LEFT ? UA1000_RIGHT
						     : UA1000_LEFT;

	edit->count = 0;
	state->monitor_gain[side] = gain;
	ua1000_build_monitor_gain(&edit->msg[edit->count++], side, gain);
	if (state->monitor_link) {
		state->monitor_gain[other] = gain;
		ua1000_build_monitor_gain(&edit->msg[edit->count++], other,
					  gain);
	}
	return 0;
}

/*
 * Link or unlink MONITOR OUT's sides.  Linking does not equalise their gains:
 * the next ua1000_edit_monitor_gain() does.
 */
int ua1000_edit_monitor_link(struct ua1000_state *state,
			     struct ua1000_edit *edit, bool link)
{
	state->monitor_link = link;
	edit->count = 1;
	return ua1000_build_monitor_link(&edit->msg[0], link);
}

/* Write the panel switches as one block. */
int ua1000_edit_panel(struct ua1000_state *state, struct ua1000_edit *edit,
		      const struct ua1000_panel *panel)
{
	struct ua1000_msg msg;
	const struct ua1000_panel *old = &state->panel;
	bool adat_on = (panel->digital_out_adat && !old->digital_out_adat) ||
		       (panel->clock == UA1000_CLOCK_EXT_ADAT &&
			old->clock != UA1000_CLOCK_EXT_ADAT);

	/*
	 * Turning ADAT on at a rate the device ignores it at is refused with
	 * -ERANGE.  Other edits pass, so a state saved before this rule with
	 * ADAT on can still be changed, ADAT off included.
	 */
	if (adat_on && !ua1000_rate_allows_adat(panel->rate)) {
		edit->count = 0;
		return -ERANGE;
	}
	if (ua1000_build_panel(&msg, panel) != 0)
		return refuse(edit);
	state->panel = *panel;
	edit->count = 1;
	edit->msg[0] = msg;
	return 0;
}

/* Route source to one pair of a patch-bay table. */
int ua1000_edit_route(struct ua1000_state *state, struct ua1000_edit *edit,
		      enum ua1000_route_table table, unsigned int pair,
		      enum ua1000_source source)
{
	if (ua1000_build_route(&edit->msg[0], table, pair, source) != 0)
		return refuse(edit);
	state->patchbay.route[table][pair] = (uint8_t)source;
	edit->count = 1;
	return 0;
}

/*
 * Apply a patch-bay preset to WAVE IN and/or OUTPUT (KEEP leaves a table as
 * it is).  The state takes the tables the preset leaves on the device, as
 * ua1000_apply_*_preset() model them.
 */
int ua1000_edit_preset(struct ua1000_state *state, struct ua1000_edit *edit,
		       enum ua1000_wave_in_preset wave_in,
		       enum ua1000_output_preset output)
{
	if (ua1000_build_preset(&edit->msg[0], wave_in, output) != 0)
		return refuse(edit);
	ua1000_apply_wave_in_preset(&state->patchbay, wave_in);
	ua1000_apply_output_preset(&state->patchbay, output);
	edit->count = 1;
	return 0;
}

/* Store the selected sections into a memory slot (1..5). */
int ua1000_edit_store(struct ua1000_state *state, struct ua1000_edit *edit,
		      unsigned int slot, unsigned int sections)
{
	if (ua1000_build_memory(&edit->msg[0], UA1000_MEMORY_STORE, slot,
				sections) != 0)
		return refuse(edit);

	struct ua1000_memory *memory = &state->memory[slot - 1];

	/* MONITOR is the mixer section (docs/PROTOCOL.md, Memory): a store
	 * without it leaves the slot's mixer, and what is known of it, as it
	 * was. */
	if (sections & UA1000_SECTION_MONITOR) {
		memory->known = true;
		memcpy(memory->mixer, state->mixer, sizeof(memory->mixer));
		memcpy(memory->monitor_gain, state->monitor_gain,
		       sizeof(memory->monitor_gain));
		memory->monitor_link = state->monitor_link;
	}
	edit->count = 1;
	return 0;
}

/* Load the selected sections of a memory slot (1..5). */
int ua1000_edit_load(struct ua1000_state *state, struct ua1000_edit *edit,
		     unsigned int slot, unsigned int sections,
		     bool *mixer_known)
{
	if (ua1000_build_memory(&edit->msg[0], UA1000_MEMORY_LOAD, slot,
				sections) != 0)
		return refuse(edit);

	const struct ua1000_memory *memory = &state->memory[slot - 1];

	/* Without the MONITOR section the device keeps its mixer, so the
	 * state still describes it. */
	*mixer_known = true;
	if (sections & UA1000_SECTION_MONITOR) {
		*mixer_known = memory->known;
		state->mixer_unknown = !memory->known;
		if (memory->known) {
			memcpy(state->mixer, memory->mixer,
			       sizeof(state->mixer));
			/* a slot written by an older editor may hold
			 * two solos in an unlinked pair */
			one_solo_per_pair(state->mixer, COUNT(state->mixer));
			memcpy(state->monitor_gain, memory->monitor_gain,
			       sizeof(state->monitor_gain));
			state->monitor_link = memory->monitor_link;
		}
	}
	/* The panel and patch bay arrive as a dump if the load changes them. */
	edit->count = 1;
	return 0;
}

/* ---- Reading the device ---- */

/* Byte offsets in the block, as mapped on the device (docs/PROTOCOL.md). */
#define BLOCK_HEADER_LEN 4
#define BLOCK_DM_OFF 17
#define BLOCK_MONITOR_LINK 24
#define BLOCK_LINKS 25 /* input 25-26, output 27-28: 4 pairs a byte */
#define BLOCK_MUTES 30 /* input 30-32, output 33-35: 4 channels a byte */
#define BLOCK_PANS 36 /* input 36-75, output 76-115: 4 nibbles each */
#define BLOCK_MONITOR_GAIN 116 /* left, right: 6 nibbles each */
#define BLOCK_GAINS 128 /* input 128-187, output 188-247 */
#define BLOCK_PAN_NIBBLES 4
#define BLOCK_GAIN_NIBBLES 6

/*
 * Flag i of a bit field that packs four flags a byte, in bits 0-3, starting
 * at data[base] (the links and the mutes of the block).
 */
static bool block_bit(const uint8_t *data, unsigned int base, unsigned int i)
{
	return data[base + i / 4] & (1u << (i % 4));
}

/* Nibbles, most significant first; false if a byte is not a nibble. */
static bool block_nibbles(const uint8_t *data, unsigned int at,
			  unsigned int count, uint32_t *value)
{
	*value = 0;
	for (unsigned int i = 0; i < count; i++) {
		if (data[at + i] > 0x0F)
			return false;
		*value = (*value << 4) | data[at + i];
	}
	return true;
}

/* Decode the device's current-state block (UA1000_BLOCK_CURRENT). */
int ua1000_state_from_block(struct ua1000_state *state,
			    const struct ua1000_dt1 *dt1)
{
	static const uint8_t header[BLOCK_HEADER_LEN] = { 1, 1, 1, 1 };
	const uint8_t *data = dt1->data;
	struct ua1000_event event = { 0 };
	struct ua1000_state st;

	/* Address 02 00 00 00, the whole block, and the header the device
	 * answers a request with. */
	if (dt1->addr[0] != 0x02 || dt1->addr[1] != UA1000_BLOCK_CURRENT ||
	    dt1->addr[2] != 0x00 || dt1->addr[3] != 0x00 ||
	    dt1->len != UA1000_GLOBAL_BLOCK_SIZE ||
	    memcmp(data, header, sizeof(header)) != 0)
		return -EBADMSG;
	/* Decoded into a fresh state, so that *state is untouched when the
	 * block is refused. */
	ua1000_state_init(&st);
	/* panel, WAVE IN and DM ON: as the device's own dumps */
	event.panel = st.panel;
	event.patchbay = st.patchbay;
	ua1000_decode(dt1, &event);
	if (event.type != UA1000_EVENT_GLOBAL)
		return -EBADMSG;
	st.panel = event.panel;
	st.patchbay = event.patchbay;
	/* DM OFF is in the block only, not in the dumps. */
	for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++) {
		uint8_t source = data[BLOCK_DM_OFF + pair];

		if (!ua1000_source_valid(UA1000_ROUTE_OUTPUT_DM_OFF, source))
			return -EBADMSG;
		st.patchbay.route[UA1000_ROUTE_OUTPUT_DM_OFF][pair] = source;
	}
	st.monitor_link = data[BLOCK_MONITOR_LINK] != 0;
	/* Both mixers have the same layout, the output's after the input's. */
	for (unsigned int m = 0; m < 2; m++) {
		struct ua1000_mixer_state *mixer = &st.mixer[m];

		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			mixer->link[pair] =
				block_bit(data, BLOCK_LINKS + 2 * m, pair);
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++) {
			struct ua1000_strip *s = &mixer->strip[ch];
			unsigned int n = m * UA1000_CHANNELS + ch;
			uint32_t pan, gain;

			if (!block_nibbles(data,
					   BLOCK_PANS + BLOCK_PAN_NIBBLES * n,
					   BLOCK_PAN_NIBBLES, &pan) ||
			    pan > UA1000_PAN_RAW_MAX ||
			    !block_nibbles(data,
					   BLOCK_GAINS + BLOCK_GAIN_NIBBLES * n,
					   BLOCK_GAIN_NIBBLES, &gain) ||
			    gain > UA1000_GAIN_MAX)
				return -EBADMSG;
			s->mute = block_bit(data, BLOCK_MUTES + 3 * m, ch);
			s->pan = (uint16_t)pan;
			s->gain = gain;
			/* not decoded: bytes 22-23 hold only the last solo
			 * written to each mixer, not one per pair */
			s->solo = false;
		}
	}
	for (unsigned int side = 0; side < 2; side++) {
		uint32_t gain;

		if (!block_nibbles(data,
				   BLOCK_MONITOR_GAIN +
					   BLOCK_GAIN_NIBBLES * side,
				   BLOCK_GAIN_NIBBLES, &gain) ||
		    gain > UA1000_GAIN_MAX)
			return -EBADMSG;
		st.monitor_gain[side] = gain;
	}
	*state = st;
	return 0;
}

/*
 * Take what a dump from the device reports.  A GLOBAL dump carries the panel
 * and the WAVE IN and DM ON tables only, so DM OFF and the mixers are kept.
 */
void ua1000_state_apply_event(struct ua1000_state *state,
			      const struct ua1000_event *event)
{
	switch (event->type) {
	case UA1000_EVENT_UNKNOWN:
		break;
	case UA1000_EVENT_PANEL:
		state->panel = event->panel;
		break;
	case UA1000_EVENT_GLOBAL:
		state->panel = event->panel;
		memcpy(state->patchbay.route[UA1000_ROUTE_WAVE_IN],
		       event->patchbay.route[UA1000_ROUTE_WAVE_IN],
		       UA1000_PAIRS);
		memcpy(state->patchbay.route[UA1000_ROUTE_OUTPUT_DM_ON],
		       event->patchbay.route[UA1000_ROUTE_OUTPUT_DM_ON],
		       UA1000_PAIRS);
		break;
	}
}

/* Every message that brings the device to *state. */
void ua1000_state_sync(const struct ua1000_state *state,
		       struct ua1000_sync *sync)
{
	sync->count = 0;
	ua1000_build_panel(&sync->msg[sync->count++], &state->panel);
	for (int t = UA1000_ROUTE_WAVE_IN; t <= UA1000_ROUTE_OUTPUT_DM_OFF; t++)
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			ua1000_build_route(&sync->msg[sync->count++],
					   (enum ua1000_route_table)t, pair,
					   (enum ua1000_source)state->patchbay
						   .route[t][pair]);
	for (int m = UA1000_MIXER_INPUT; m <= UA1000_MIXER_OUTPUT; m++) {
		const struct ua1000_mixer_state *mixer = &state->mixer[m];
		enum ua1000_mixer id = (enum ua1000_mixer)m;

		/* Links first: they decide how the device reads the pair. */
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			ua1000_build_link(&sync->msg[sync->count++], id, pair,
					  mixer->link[pair]);
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++) {
			const struct ua1000_strip *s = &mixer->strip[ch];

			ua1000_build_gain(&sync->msg[sync->count++], id, ch,
					  s->gain);
			ua1000_build_pan(&sync->msg[sync->count++], id, ch,
					 s->pan);
			ua1000_build_mute(&sync->msg[sync->count++], id, ch,
					  s->mute);
		}
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			ua1000_build_solo(&sync->msg[sync->count++], id, pair,
					  solo_mask(state, id, pair));
	}
	ua1000_build_monitor_link(&sync->msg[sync->count++],
				  state->monitor_link);
	ua1000_build_monitor_gain(&sync->msg[sync->count++], UA1000_LEFT,
				  state->monitor_gain[UA1000_LEFT]);
	ua1000_build_monitor_gain(&sync->msg[sync->count++], UA1000_RIGHT,
				  state->monitor_gain[UA1000_RIGHT]);
}

/*
 * Whether two states write the same state file: a comparison of everything
 * the file holds, without listing the fields twice.  A state that cannot be
 * written to memory counts as different.  Used by
 * ua1000_state_adopt_device().
 */
static bool same_text(const struct ua1000_state *a,
		      const struct ua1000_state *b)
{
	char *text[2] = { NULL, NULL };
	size_t len[2] = { 0, 0 };
	const struct ua1000_state *st[2] = { a, b };
	bool same;

	for (int i = 0; i < 2; i++) {
		FILE *f = open_memstream(&text[i], &len[i]);

		if (f) {
			ua1000_state_write(st[i], f);
			fclose(f);
		}
	}
	same = text[0] && text[1] && len[0] == len[1] &&
	       memcmp(text[0], text[1], len[0]) == 0;
	free(text[0]);
	free(text[1]);
	return same;
}

/* Make the device's state the editor's, keeping solos, memories and rate. */
bool ua1000_state_adopt_device(struct ua1000_state *state,
			       const struct ua1000_state *device,
			       struct ua1000_sync *solos)
{
	struct ua1000_state next = *device;
	bool differed;

	memcpy(next.memory, state->memory, sizeof(next.memory));
	/* the running rate is the driver's (the caller's): the block's byte 4
	 * holds the position of the SAMPLE RATE switch, not a rate code
	 * (docs/PROTOCOL.md, Reading the device) */
	next.panel.rate = state->panel.rate;
	for (unsigned int m = 0; m < 2; m++)
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++)
			next.mixer[m].strip[ch].solo =
				state->mixer[m].strip[ch].solo;
	/* the links are the device's now: one solo per unlinked pair under
	 * them, as the device reads the pair */
	one_solo_per_pair(next.mixer, COUNT(next.mixer));
	next.mixer_unknown = false;
	{
		/* compared as the state file writes them, with what the device
		 * cannot report (solos, memories) and the running rate (the
		 * driver's) taken from next */
		struct ua1000_state a = *state;

		a.mixer_unknown = false;
		a.panel.rate = next.panel.rate;
		memcpy(a.memory, next.memory, sizeof(a.memory));
		for (unsigned int m = 0; m < 2; m++)
			for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++)
				a.mixer[m].strip[ch].solo =
					next.mixer[m].strip[ch].solo;
		differed = !same_text(&a, &next);
	}
	*state = next;
	/* The block does not say which channels are soloed: the editor's
	 * solos are written over whatever the device holds. */
	solos->count = 0;
	for (int m = UA1000_MIXER_INPUT; m <= UA1000_MIXER_OUTPUT; m++)
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			ua1000_build_solo(&solos->msg[solos->count++],
					  (enum ua1000_mixer)m, pair,
					  solo_mask(state, (enum ua1000_mixer)m,
						    pair));
	return differed;
}

/* ---- Describing a change ---- */

/* The output buffer of ua1000_state_describe_changes() as it fills. */
struct changes {
	char *out;
	size_t len, used, count, max_lines;
};

/*
 * Count one change and, while fewer than max_lines have been written and
 * the buffer has room, append its line.  Truncates silently; the buffer
 * stays NUL-terminated.
 */
static void change(struct changes *c, const char *fmt, ...)
{
	va_list ap;
	int n;

	c->count++;
	if (c->count > c->max_lines || c->used >= c->len)
		return;
	va_start(ap, fmt);
	n = vsnprintf(c->out + c->used, c->len - c->used, fmt, ap);
	va_end(ap);
	if (n > 0)
		c->used += (size_t)n;
	if (c->used >= c->len)
		c->used = c->len - 1;
}

/*
 * A gain in dB with one decimal, as the changes show it: "-inf" for silence,
 * and no "-0.0".  Two gains are reported as different only when this text
 * differs.
 */
static void db_text(uint32_t gain, char *buf, size_t len)
{
	double db = ua1000_gain_to_db(gain);

	if (isinf(db))
		snprintf(buf, len, "-inf");
	else
		snprintf(buf, len, "%.1f", fabs(db) < 0.05 ? 0.0 : db);
}

/* A pan position as the panel shows it: "C", "L<n>" or "R<n>". */
static void pan_text(uint16_t raw, char *buf, size_t len)
{
	int p = ua1000_pan_to_position(raw);

	if (p == 0)
		snprintf(buf, len, "C");
	else
		snprintf(buf, len, "%c%d", p < 0 ? 'L' : 'R', abs(p));
}

/* The clock source as the device's CLOCK SOURCE switch labels it. */
static const char *clock_label(enum ua1000_clock clock)
{
	return clock == UA1000_CLOCK_EXT_SPDIF	? "S/PDIF"
	       : clock == UA1000_CLOCK_EXT_ADAT ? "ADAT"
						: "INT";
}

/* Report an on/off setting when it changed. */
static void describe_flag(struct changes *c, const char *what, bool from,
			  bool to)
{
	if (from != to)
		change(c, "%s: %s \u2192 %s\n", what, from ? "on" : "off",
		       to ? "on" : "off");
}

/* The differences from *from to *to, one line each, in the panel's words. */
size_t ua1000_state_describe_changes(const struct ua1000_state *from,
				     const struct ua1000_state *to, char *out,
				     size_t len, size_t max_lines)
{
	static const char *const mixer_name[2] = { "Input", "Output" };
	static const char *const table_name[3] = { "WAVE IN", "OUTPUT (DM on)",
						   "OUTPUT (DM off)" };
	const struct ua1000_panel *a = &from->panel, *b = &to->panel;
	struct changes c = { out, len, 0, 0, max_lines };
	char x[16], y[16];

	if (len == 0)
		return 0;
	out[0] = '\0';
	if (a->clock != b->clock)
		change(&c, "CLOCK SOURCE: %s \u2192 %s\n",
		       clock_label(a->clock), clock_label(b->clock));
	if (a->digital_out_adat != b->digital_out_adat)
		change(&c, "DIGITAL OUT: %s \u2192 %s\n",
		       a->digital_out_adat ? "ADAT" : "S/PDIF",
		       b->digital_out_adat ? "ADAT" : "S/PDIF");
	if (a->output_all != b->output_all)
		change(&c, "OUTPUT VOLUME: %s \u2192 %s\n",
		       a->output_all ? "ALL" : "MAIN",
		       b->output_all ? "ALL" : "MAIN");
	describe_flag(&c, "MONO", a->mono, b->mono);
	describe_flag(&c, "SOFT CTRL", a->soft_ctrl, b->soft_ctrl);
	for (unsigned int t = 0; t < 3; t++)
		for (unsigned int p = 0; p < UA1000_PAIRS; p++)
			if (from->patchbay.route[t][p] !=
			    to->patchbay.route[t][p])
				change(&c, "%s %u-%u: %s \u2192 %s\n",
				       table_name[t], 2 * p + 1, 2 * p + 2,
				       ua1000_source_name(
					       (enum ua1000_source)from
						       ->patchbay.route[t][p]),
				       ua1000_source_name(
					       (enum ua1000_source)to->patchbay
						       .route[t][p]));
	for (unsigned int m = 0; m < 2; m++) {
		for (unsigned int p = 0; p < UA1000_PAIRS; p++)
			if (from->mixer[m].link[p] != to->mixer[m].link[p])
				change(&c, "%s %u-%u link: %s \u2192 %s\n",
				       mixer_name[m], 2 * p + 1, 2 * p + 2,
				       from->mixer[m].link[p] ? "on" : "off",
				       to->mixer[m].link[p] ? "on" : "off");
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++) {
			const struct ua1000_strip *s =
				&from->mixer[m].strip[ch];
			const struct ua1000_strip *t = &to->mixer[m].strip[ch];

			db_text(s->gain, x, sizeof(x));
			db_text(t->gain, y, sizeof(y));
			if (strcmp(x, y) != 0)
				change(&c, "%s %u: gain %s \u2192 %s dB\n",
				       mixer_name[m], ch + 1, x, y);
			pan_text(s->pan, x, sizeof(x));
			pan_text(t->pan, y, sizeof(y));
			if (strcmp(x, y) != 0)
				change(&c, "%s %u: pan %s \u2192 %s\n",
				       mixer_name[m], ch + 1, x, y);
			if (s->mute != t->mute)
				change(&c, "%s %u: %s\n", mixer_name[m], ch + 1,
				       t->mute ? "muted" : "unmuted");
		}
	}
	for (unsigned int side = 0; side < 2; side++) {
		db_text(from->monitor_gain[side], x, sizeof(x));
		db_text(to->monitor_gain[side], y, sizeof(y));
		if (strcmp(x, y) != 0)
			change(&c, "MONITOR OUT %s: gain %s \u2192 %s dB\n",
			       side == UA1000_LEFT ? "L" : "R", x, y);
	}
	describe_flag(&c, "MONITOR OUT link", from->monitor_link,
		      to->monitor_link);
	/* Only when there is room left: the lines already written stand. */
	if (c.count > max_lines && c.used + 1 < len)
		snprintf(out + c.used, len - c.used, "and %zu more\n",
			 c.count - max_lines);
	return c.count;
}

/* ---- State file ---- */

/* A boolean as the state file spells it. */
static const char *on_off(bool value)
{
	return value ? "on" : "off";
}

/*
 * Write both mixers and MONITOR OUT, each line led by prefix: "" for the
 * live state, "memory N " for a memory slot, so that both are read back by
 * the same parse_mixer_line().
 */
static void write_mixer(FILE *f, const char *prefix,
			const struct ua1000_mixer_state mixer[2],
			const uint32_t monitor_gain[2], bool monitor_link)
{
	for (int m = UA1000_MIXER_INPUT; m <= UA1000_MIXER_OUTPUT; m++) {
		const char *name = ua1000_mixer_name((enum ua1000_mixer)m);

		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++) {
			const struct ua1000_strip *s = &mixer[m].strip[ch];

			fprintf(f, "%sstrip %s %u gain %lu pan %u mute %s solo %s\n",
				prefix, name, ch + 1, (unsigned long)s->gain,
				s->pan, on_off(s->mute), on_off(s->solo));
		}
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			fprintf(f, "%slink %s %u %s\n", prefix, name, pair + 1,
				on_off(mixer[m].link[pair]));
	}
	fprintf(f, "%smonitor gain-left %lu gain-right %lu link %s\n", prefix,
		(unsigned long)monitor_gain[UA1000_LEFT],
		(unsigned long)monitor_gain[UA1000_RIGHT],
		on_off(monitor_link));
}

/*
 * Write the state file: header, panel, routes, the mixers, the mixer_unknown
 * mark, then each memory this editor stored, its lines prefixed "memory N".
 */
int ua1000_state_write(const struct ua1000_state *state, FILE *f)
{
	const struct ua1000_panel *p = &state->panel;

	fprintf(f, "%s %d\n", STATE_MAGIC, STATE_VERSION);
	fprintf(f, "rate %u\n", ua1000_rate_hz(p->rate));
	fprintf(f, "clock %s\n", ua1000_clock_name(p->clock));
	fprintf(f, "digital-out %s\n", p->digital_out_adat ? "adat" : "spdif");
	fprintf(f, "output-volume %s\n", p->output_all ? "all" : "main");
	fprintf(f, "mono %s\n", on_off(p->mono));
	fprintf(f, "soft-ctrl %s\n", on_off(p->soft_ctrl));
	for (int t = UA1000_ROUTE_WAVE_IN; t <= UA1000_ROUTE_OUTPUT_DM_OFF; t++)
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			fprintf(f, "route %s %u %s\n",
				ua1000_route_table_name(
					(enum ua1000_route_table)t),
				pair + 1,
				ua1000_source_name(
					(enum ua1000_source)state->patchbay
						.route[t][pair]));
	write_mixer(f, "", state->mixer, state->monitor_gain,
		    state->monitor_link);
	if (state->mixer_unknown)
		fputs("mixer unknown\n", f);
	for (unsigned int slot = 1; slot <= UA1000_MEMORY_SLOTS; slot++) {
		const struct ua1000_memory *mem = &state->memory[slot - 1];
		char prefix[16];

		if (!mem->known)
			continue;
		snprintf(prefix, sizeof(prefix), "memory %u ", slot);
		write_mixer(f, prefix, mem->mixer, mem->monitor_gain,
			    mem->monitor_link);
	}
	return ferror(f) ? -EIO : 0;
}

/* A parser over the whitespace-separated words of one line. */
struct line {
	char *word[16];
	size_t count;
	size_t next;
	char *err;
	size_t err_len;
};

/*
 * Record why the line is refused in l->err and return false, so that a
 * parser can "return fail(...)".  ua1000_state_read() adds the line number.
 */
static bool fail(struct line *l, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(l->err, l->err_len, fmt, ap);
	va_end(ap);
	return false;
}

/* The next word of the line, or NULL at its end. */
static const char *word(struct line *l)
{
	return l->next < l->count ? l->word[l->next++] : NULL;
}

/* Consume the next word, which must be keyword. */
static bool expect(struct line *l, const char *keyword)
{
	const char *w = word(l);

	if (!w || strcmp(w, keyword) != 0)
		return fail(l, "expected \"%s\"", keyword);
	return true;
}

/*
 * Consume a decimal number in min..max.  A minus sign and trailing
 * characters are refused.  On failure *value is still written, possibly out
 * of range: callers must not use it.
 */
static bool number(struct line *l, unsigned long min, unsigned long max,
		   unsigned long *value)
{
	const char *w = word(l);
	char *end;

	*value = 0;
	if (!w)
		return fail(l, "missing number");
	errno = 0;
	*value = strtoul(w, &end, 10);
	if (errno || *end || w[0] == '-' || *value < min || *value > max) {
		/* Not "return fail()": the analyzer does not follow a variadic
		 * call, and would then index with an unchecked value. */
		(void)fail(l, "\"%s\" is not a number in %lu..%lu", w, min,
			   max);
		return false;
	}
	return true;
}

/* Consume one of two words: on gives true, off false. */
static bool boolean(struct line *l, const char *on, const char *off,
		    bool *value)
{
	const char *w = word(l);

	*value = false;
	if (w && strcmp(w, on) == 0)
		*value = true;
	else if (w && strcmp(w, off) == 0)
		*value = false;
	else
		return fail(l, "expected \"%s\" or \"%s\"", on, off);
	return true;
}

/* Consume "on" or "off". */
static bool flag(struct line *l, bool *value)
{
	return boolean(l, "on", "off", value);
}

/*
 * Consume a mixer name.  *mixer is always left a valid index, so that a
 * caller cannot index with garbage even on failure.
 */
static bool mixer_word(struct line *l, enum ua1000_mixer *mixer)
{
	const char *w = word(l);

	*mixer = UA1000_MIXER_INPUT;
	if (!w || !ua1000_mixer_parse(w, mixer))
		return fail(l, "expected \"input\" or \"output\"");
	return true;
}

/*
 * "strip MIXER CH gain G pan P mute on|off solo on|off": one strip, written
 * only when the whole line is valid.  Channels are 1-based in the file.
 */
static bool parse_strip(struct line *l, struct ua1000_mixer_state mixer[2])
{
	enum ua1000_mixer m;
	unsigned long ch, gain, pan;
	struct ua1000_strip s;

	if (!mixer_word(l, &m) || !number(l, 1, UA1000_CHANNELS, &ch) ||
	    !expect(l, "gain") || !number(l, 0, UA1000_GAIN_MAX, &gain) ||
	    !expect(l, "pan") || !number(l, 0, UA1000_PAN_RAW_MAX, &pan) ||
	    !expect(l, "mute") || !flag(l, &s.mute) || !expect(l, "solo") ||
	    !flag(l, &s.solo))
		return false;
	s.gain = (uint32_t)gain;
	s.pan = (uint16_t)pan;
	mixer[m].strip[ch - 1] = s;
	return true;
}

/* "link MIXER PAIR on|off", pairs 1-based. */
static bool parse_link(struct line *l, struct ua1000_mixer_state mixer[2])
{
	enum ua1000_mixer m;
	unsigned long pair;

	return mixer_word(l, &m) && number(l, 1, UA1000_PAIRS, &pair) &&
	       flag(l, &mixer[m].link[pair - 1]);
}

/* "monitor gain-left G gain-right G link on|off". */
static bool parse_monitor(struct line *l, uint32_t gain[2], bool *link)
{
	unsigned long left, right;

	if (!expect(l, "gain-left") || !number(l, 0, UA1000_GAIN_MAX, &left) ||
	    !expect(l, "gain-right") ||
	    !number(l, 0, UA1000_GAIN_MAX, &right) || !expect(l, "link") ||
	    !flag(l, link))
		return false;
	gain[UA1000_LEFT] = (uint32_t)left;
	gain[UA1000_RIGHT] = (uint32_t)right;
	return true;
}

/*
 * The mixer settings, key already consumed: shared by the live state and
 * the "memory N" lines, which give their own mixer and monitor to fill.
 * An unknown key fails here, so the whole file refuses unknown settings.
 */
static bool parse_mixer_line(struct line *l, const char *key,
			     struct ua1000_mixer_state mixer[2],
			     uint32_t monitor_gain[2], bool *monitor_link)
{
	if (strcmp(key, "strip") == 0)
		return parse_strip(l, mixer);
	if (strcmp(key, "link") == 0)
		return parse_link(l, mixer);
	if (strcmp(key, "monitor") == 0)
		return parse_monitor(l, monitor_gain, monitor_link);
	return fail(l, "unknown setting \"%s\"", key);
}

/*
 * "route TABLE PAIR SOURCE": the source must be one the table accepts
 * ("same" is DM OFF's only).
 */
static bool parse_route(struct line *l, struct ua1000_patchbay *patchbay)
{
	const char *w = word(l);
	enum ua1000_route_table table;
	enum ua1000_source source;
	unsigned long pair;

	if (!w || !ua1000_route_table_parse(w, &table))
		return fail(l, "unknown routing table");
	if (!number(l, 1, UA1000_PAIRS, &pair))
		return false;
	w = word(l);
	if (!w || !ua1000_source_parse(w, &source) ||
	    !ua1000_source_valid(table, source))
		return fail(l, "\"%s\" is not a source of this table",
			    w ? w : "");
	patchbay->route[table][pair - 1] = (uint8_t)source;
	return true;
}

/*
 * One line after the header, its words already split: the panel settings
 * are handled here, the rest by parse_route() and parse_mixer_line().
 * Called by ua1000_state_read(), which checks afterwards that every word
 * was consumed.
 */
static bool parse_setting(struct line *l, struct ua1000_state *state)
{
	struct ua1000_panel *p = &state->panel;
	const char *key = word(l);
	unsigned long value;

	if (strcmp(key, "rate") == 0) {
		if (!number(l, 0, 1000000, &value))
			return false;
		if (!ua1000_rate_from_hz((unsigned int)value, &p->rate))
			return fail(l, "unsupported sample rate %lu", value);
		return true;
	}
	if (strcmp(key, "clock") == 0) {
		const char *w = word(l);

		if (!w || !ua1000_clock_parse(w, &p->clock))
			return fail(l, "unknown clock source");
		return true;
	}
	if (strcmp(key, "digital-out") == 0)
		return boolean(l, "adat", "spdif", &p->digital_out_adat);
	if (strcmp(key, "output-volume") == 0)
		return boolean(l, "all", "main", &p->output_all);
	if (strcmp(key, "mono") == 0)
		return flag(l, &p->mono);
	if (strcmp(key, "soft-ctrl") == 0)
		return flag(l, &p->soft_ctrl);
	if (strcmp(key, "route") == 0)
		return parse_route(l, &state->patchbay);
	if (strcmp(key, "mixer") == 0) {
		const char *w = word(l);

		if (!w || strcmp(w, "unknown") != 0)
			return fail(l, "expected \"mixer unknown\"");
		state->mixer_unknown = true;
		return true;
	}
	if (strcmp(key, "memory") == 0) {
		if (!number(l, 1, UA1000_MEMORY_SLOTS, &value))
			return false;

		struct ua1000_memory *mem = &state->memory[value - 1];
		const char *sub = word(l);

		if (!sub)
			return fail(l, "missing setting");
		/* The first line of a slot starts it from the defaults, so a
		 * slot is known however few of its lines the file holds. */
		if (!mem->known) {
			mem->known = true;
			mixer_init(&mem->mixer[0]);
			mixer_init(&mem->mixer[1]);
			mem->monitor_gain[0] = mem->monitor_gain[1] =
				UA1000_GAIN_UNITY;
			mem->monitor_link = true;
		}
		return parse_mixer_line(l, sub, mem->mixer, mem->monitor_gain,
					&mem->monitor_link);
	}
	return parse_mixer_line(l, key, state->mixer, state->monitor_gain,
				&state->monitor_link);
}

/* Parse a state file into *state, from the defaults. */
int ua1000_state_read(struct ua1000_state *state, FILE *f, char *err,
		      size_t err_len)
{
	char buf[256];
	unsigned int number_of_line = 0;
	bool header_seen = false;

	ua1000_state_init(state);
	while (fgets(buf, sizeof(buf), f)) {
		struct line l = { .err = err, .err_len = err_len };
		char *save = NULL;

		number_of_line++;
		if (strlen(buf) + 1 < sizeof(buf) && !strchr(buf, '\n') &&
		    !feof(f)) {
			/* fgets stopped early with no newline: a NUL byte */
			snprintf(err, err_len, "line %u: not text (a NUL byte)",
				 number_of_line);
			return -EINVAL;
		}
		if (!strchr(buf, '\n') && !feof(f)) {
			int next = getc(f);

			/* a full buffer at the very end of the file fits */
			if (next != EOF) {
				snprintf(err, err_len, "line %u: too long",
					 number_of_line);
				return -EINVAL;
			}
		}
		bool ok = true;

		/* Split into words; a line starting with '#' is a comment. */
		for (char *w = strtok_r(buf, " \t\r\n", &save); w;
		     w = strtok_r(NULL, " \t\r\n", &save)) {
			if (l.count == COUNT(l.word)) {
				ok = fail(&l, "too many words");
				break;
			}
			l.word[l.count++] = w;
		}
		if (ok && (l.count == 0 || l.word[0][0] == '#'))
			continue;

		if (!ok) {
			/* reported below */
		} else if (!header_seen) {
			unsigned long version;

			ok = expect(&l, STATE_MAGIC) &&
			     number(&l, 0, ULONG_MAX, &version);
			if (ok && version != STATE_VERSION)
				ok = fail(&l,
					  "format version %lu, this program "
					  "reads version %d",
					  version, STATE_VERSION);
			header_seen = true;
		} else {
			ok = parse_setting(&l, state);
		}
		if (ok && l.next != l.count)
			ok = fail(&l, "unexpected \"%s\"", l.word[l.next]);
		if (!ok) {
			/* err is both the reason and the result: copied
			 * first, as snprintf() may not read and write the same
			 * buffer. */
			char reason[128];

			snprintf(reason, sizeof(reason), "%s", err);
			snprintf(err, err_len, "line %u: %s", number_of_line,
				 reason);
			return -EINVAL;
		}
	}
	if (!header_seen) {
		snprintf(err, err_len, "no \"%s\" header", STATE_MAGIC);
		return -EINVAL;
	}
	/* a file written by an older editor, or by hand, may hold two
	 * solos in an unlinked pair */
	one_solo_per_pair(state->mixer, COUNT(state->mixer));
	return ferror(f) ? -EIO : 0;
}
