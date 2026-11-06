/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000_state.h - what the editor knows about the device, and how to change it
 *
 * The UA-1000 cannot be asked for its mixer settings; it only volunteers its
 * panel switches and patch bay (ua1000_proto.h, UA1000_EVENT_*).  So the
 * editor keeps its own copy of everything it has set, shared between the
 * command-line tool and the GUI through one state file.
 *
 * Every change goes through a ua1000_edit_*() function, which updates the
 * state and returns the messages that make the device follow, including the
 * partner channel of a linked pair.  Neither program builds mixer messages
 * on its own, so the two cannot disagree about what a change means.  Only
 * one of them can hold the device's MIDI port at a time.
 */
#ifndef UA1000_STATE_H
#define UA1000_STATE_H

#include "ua1000_proto.h"

#include <stdio.h>

struct ua1000_strip {
	uint32_t gain;
	uint16_t pan;
	bool mute;
	bool solo;
};

struct ua1000_mixer_state {
	struct ua1000_strip strip[UA1000_CHANNELS];
	bool link[UA1000_PAIRS];
};

/* A memory slot as this editor stored it. */
struct ua1000_memory {
	bool known; /* false: stored by someone else, or never */
	struct ua1000_mixer_state mixer[2];
	uint32_t monitor_gain[2];
	bool monitor_link;
};

struct ua1000_state {
	struct ua1000_panel panel;
	struct ua1000_patchbay patchbay;
	struct ua1000_mixer_state mixer[2]; /* indexed by ua1000_mixer */
	uint32_t monitor_gain[2]; /* indexed by ua1000_side */
	bool monitor_link;
	struct ua1000_memory memory[UA1000_MEMORY_SLOTS]; /* slot 1 is [0] */
	/* A memory this editor did not store was loaded: the device's mixer
	 * is no longer the one in this state, until a sync sends it. */
	bool mixer_unknown;
};

/*
 * The editor's defaults: unity gain, centre pan, nothing muted, soloed or
 * linked, the monitor linked at unity, and the device's initial panel and
 * patch bay.  The device's own power-up mixer settings are not known.
 */
void ua1000_state_init(struct ua1000_state *state);

/*
 * Back to the editor's defaults, except what is not the editor's to forget:
 * what it knows of the memories, and the sample rate the device runs at.
 * ua1000_state_sync() then brings the device along.
 */
void ua1000_state_reset(struct ua1000_state *state);

/* The messages one edit produces; a linked pair needs two. */
#define UA1000_EDIT_MAX 2

struct ua1000_edit {
	size_t count;
	struct ua1000_msg msg[UA1000_EDIT_MAX];
};

/*
 * Edits.  Each returns 0 and fills *edit, or a negative errno with the state
 * untouched and edit->count set to 0: -EINVAL for a value out of range, and
 * ua1000_edit_panel() -ERANGE for ADAT at a rate the device ignores it at.
 *
 * When a channel's pair is linked, gain, mute and solo follow on its partner
 * and pan is mirrored (the partner gets the opposite position).
 */
int ua1000_edit_gain(struct ua1000_state *state, struct ua1000_edit *edit,
		     enum ua1000_mixer mixer, unsigned int channel,
		     uint32_t gain);
int ua1000_edit_pan(struct ua1000_state *state, struct ua1000_edit *edit,
		    enum ua1000_mixer mixer, unsigned int channel,
		    uint16_t pan);
int ua1000_edit_mute(struct ua1000_state *state, struct ua1000_edit *edit,
		     enum ua1000_mixer mixer, unsigned int channel, bool mute);
int ua1000_edit_solo(struct ua1000_state *state, struct ua1000_edit *edit,
		     enum ua1000_mixer mixer, unsigned int channel, bool solo);
int ua1000_edit_link(struct ua1000_state *state, struct ua1000_edit *edit,
		     enum ua1000_mixer mixer, unsigned int pair, bool link);

int ua1000_edit_monitor_gain(struct ua1000_state *state,
			     struct ua1000_edit *edit, enum ua1000_side side,
			     uint32_t gain);
int ua1000_edit_monitor_link(struct ua1000_state *state,
			     struct ua1000_edit *edit, bool link);

/* The panel switches, written together as one block. */
int ua1000_edit_panel(struct ua1000_state *state, struct ua1000_edit *edit,
		      const struct ua1000_panel *panel);
int ua1000_edit_route(struct ua1000_state *state, struct ua1000_edit *edit,
		      enum ua1000_route_table table, unsigned int pair,
		      enum ua1000_source source);
int ua1000_edit_preset(struct ua1000_state *state, struct ua1000_edit *edit,
		       enum ua1000_wave_in_preset wave_in,
		       enum ua1000_output_preset output);

/*
 * Storing a slot remembers the mixer in the state, so that loading it later
 * can restore what the device will not report.  Loading returns, in
 * *mixer_known, whether the slot's mixer settings are known; when they are
 * not, the state's mixer no longer describes the device, and the state says
 * so (mixer_unknown) until a sync or a load of a known slot.
 */
int ua1000_edit_store(struct ua1000_state *state, struct ua1000_edit *edit,
		      unsigned int slot, unsigned int sections);
int ua1000_edit_load(struct ua1000_state *state, struct ua1000_edit *edit,
		     unsigned int slot, unsigned int sections,
		     bool *mixer_known);

/*
 * The device's state from its answer to ua1000_build_block_request() for
 * UA1000_BLOCK_CURRENT (docs/PROTOCOL.md, Reading the device): panel, the
 * three patch-bay tables, links, mutes, pans and gains of both mixers and
 * MONITOR OUT.  The solos are not decoded (their encoding is unknown): they
 * are left off, as are the memories.  Returns 0, or -EBADMSG when the
 * message is not that block or holds a value out of range.
 */
int ua1000_state_from_block(struct ua1000_state *state,
			    const struct ua1000_dt1 *dt1);

/* Apply what the device reported (ua1000_decode()). */
void ua1000_state_apply_event(struct ua1000_state *state,
			      const struct ua1000_event *event);

/* All the messages that bring the device to the state, in a sane order. */
struct ua1000_sync {
	size_t count;
	/* panel, routes, both mixers (gain, pan, mute; link, solo), monitor */
	struct ua1000_msg msg[1 + 3 * UA1000_PAIRS +
			      2 * (3 * UA1000_CHANNELS + 2 * UA1000_PAIRS) + 3];
};

void ua1000_state_sync(const struct ua1000_state *state,
		       struct ua1000_sync *sync);

/*
 * Take the device's state (ua1000_state_from_block()) as the editor's,
 * keeping what the device cannot report: the memories this editor stored,
 * the solos, and the running sample rate (the driver's, set by the caller). The
 * device has no solo controls of its own, so the editor's solos are the true
 * ones: *solos gets the messages that give them to the device.  Returns whether
 * anything but the solos and the memories differed from *state (what the editor
 * had saved).
 */
bool ua1000_state_adopt_device(struct ua1000_state *state,
			       const struct ua1000_state *device,
			       struct ua1000_sync *solos);

/*
 * What differs between two states, in words, one change a line ("OUTPUT
 * VOLUME: ALL → MAIN", "Output 9: gain 0.0 → -0.4 dB"), at most max_lines
 * lines and then "and N more".  The solos, the memories and the running
 * rate are not compared.  Returns how many changes there are (0: the same).
 */
size_t ua1000_state_describe_changes(const struct ua1000_state *from,
				     const struct ua1000_state *to, char *out,
				     size_t len, size_t max_lines);

/*
 * Names shared by the state file and the command line, so that a setting is
 * spelled the same everywhere.  The *_parse() functions return false for an
 * unknown name.
 */
const char *ua1000_source_name(enum ua1000_source source);
bool ua1000_source_parse(const char *name, enum ua1000_source *source);
const char *ua1000_clock_name(enum ua1000_clock clock);
bool ua1000_clock_parse(const char *name, enum ua1000_clock *clock);
const char *ua1000_mixer_name(enum ua1000_mixer mixer);
bool ua1000_mixer_parse(const char *name, enum ua1000_mixer *mixer);
const char *ua1000_route_table_name(enum ua1000_route_table table);
bool ua1000_route_table_parse(const char *name, enum ua1000_route_table *table);

/*
 * The state file: plain text, one "key value..." line per setting, headed by
 * a format version.  Unknown keys are refused rather than ignored, so a typo
 * in a hand-edited file is reported instead of silently dropped.
 */
int ua1000_state_write(const struct ua1000_state *state, FILE *f);

/*
 * Read a state file into *state, which is first reset to the defaults.
 * Returns 0, or -EINVAL with a description of the first bad line in err.
 */
int ua1000_state_read(struct ua1000_state *state, FILE *f, char *err,
		      size_t err_len);

#endif /* UA1000_STATE_H */
