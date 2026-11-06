/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000_proto.h - the UA-1000 control protocol, without any I/O
 *
 * The UA-1000 is controlled with Roland "Data Set 1" (DT1) System Exclusive
 * messages on USB-MIDI cable 1.  This module builds those messages and parses
 * the ones the device sends back; it never touches a device, so every
 * function here can be tested against the captured traffic of the vendor's
 * Windows control panel.
 *
 * The address map and the provenance of every value are documented in
 * docs/PROTOCOL.md.
 */
#ifndef UA1000_PROTO_H
#define UA1000_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UA1000_CHANNELS 10
#define UA1000_PAIRS (UA1000_CHANNELS / 2)
#define UA1000_MEMORY_SLOTS 5
#define UA1000_GLOBAL_BLOCK_SIZE 256

/* F0, manufacturer, device, model (2), command, address (4) ... checksum, F7 */
#define UA1000_DT1_OVERHEAD 12
#define UA1000_SYSEX_MAX (UA1000_DT1_OVERHEAD + UA1000_GLOBAL_BLOCK_SIZE)

/* A complete SysEx message, ready to be written to the MIDI port. */
struct ua1000_msg {
	size_t len;
	uint8_t bytes[UA1000_SYSEX_MAX];
};

/* The two ten-channel monitor mixers of the device. */
enum ua1000_mixer {
	UA1000_MIXER_INPUT, /* INPUT MONITOR: the analog and digital inputs */
	UA1000_MIXER_OUTPUT, /* OUTPUT MONITOR: the computer's playback */
};

bool ua1000_mixer_valid(enum ua1000_mixer mixer);

enum ua1000_side {
	UA1000_LEFT,
	UA1000_RIGHT,
};

/*
 * Gain is a 23-bit linear amplitude: 0 is silence, 0x200000 is unity and the
 * maximum, 0x7FFFFF, is just under four times unity (+12.04 dB).
 */
#define UA1000_GAIN_UNITY 0x200000u
#define UA1000_GAIN_MAX 0x7FFFFFu

uint32_t ua1000_gain_from_db(double db);
double ua1000_gain_to_db(uint32_t gain);

/*
 * The vendor's faders have 1023 positions: 0 is silence, 766 is unity and
 * 1022 is the maximum.  These two functions reproduce the gain the vendor's
 * panel sends for each position (every captured value is within 1.3 of the
 * formula), so a fader of ours moves through the same values.
 */
#define UA1000_FADER_MAX 1022u
#define UA1000_FADER_UNITY 766u

uint32_t ua1000_fader_to_gain(unsigned int position);
unsigned int ua1000_gain_to_fader(uint32_t gain);

/*
 * Pan has 41 positions, from -20 (hard left) through 0 (centre) to +20 (hard
 * right), carried as 0x0000..0x8000.
 */
#define UA1000_PAN_STEPS 20
#define UA1000_PAN_RAW_MAX 0x8000u

uint16_t ua1000_pan_from_position(int position);
int ua1000_pan_to_position(uint16_t raw);

/* Sample-rate codes of the global block and of the USER SET. */
enum ua1000_rate {
	UA1000_RATE_44100 = 0x08,
	UA1000_RATE_48000 = 0x09,
	UA1000_RATE_88200 = 0x0A,
	UA1000_RATE_96000 = 0x0B,
};

unsigned int ua1000_rate_hz(enum ua1000_rate rate);
bool ua1000_rate_from_hz(unsigned int hz, enum ua1000_rate *rate);
/*
 * ADAT (output, or input with the ADAT clock) works at 44.1 and 48 kHz only:
 * at other rates the device ignores the ADAT switch (Owner's Manual, p. 12
 * and 41).
 */
bool ua1000_rate_allows_adat(enum ua1000_rate rate);

enum ua1000_clock {
	UA1000_CLOCK_INTERNAL = 0,
	UA1000_CLOCK_EXT_SPDIF = 1, /* S/PDIF or word clock */
	UA1000_CLOCK_EXT_ADAT = 2, /* ADAT or word clock */
};

/* The front-panel switches that software can set. */
struct ua1000_panel {
	enum ua1000_rate rate;
	enum ua1000_clock clock;
	bool digital_out_adat; /* false: S/PDIF */
	bool output_all; /* OUTPUT VOLUME knob acts on ALL outputs, not MAIN */
	bool mono; /* direct monitor summed to mono */
	bool soft_ctrl; /* direct monitor follows the software mixer */
	/*
	 * Read only.  The device sets bit 3 of its clock byte in notifications
	 * whenever an external clock is selected; whether it means "locked"
	 * or merely "external" is not known.
	 */
	bool clock_flag;
};

/*
 * The panel assumed until the device reports one: 48 kHz, internal clock,
 * S/PDIF output and SOFT CTRL on, as every dump in the captures shows, but
 * OUTPUT VOLUME on MAIN, the factory setting (Owner's Manual, p. 44).
 */
void ua1000_panel_init(struct ua1000_panel *panel);

/* Patch bay: a source for each stereo pair of three routing tables. */
enum ua1000_route_table {
	UA1000_ROUTE_WAVE_IN, /* what the computer records */
	UA1000_ROUTE_OUTPUT_DM_ON, /* what each output plays, direct monitor on */
	UA1000_ROUTE_OUTPUT_DM_OFF, /* the same, with every direct monitor off */
};

enum ua1000_source {
	UA1000_SRC_SAME_AS_DM_ON =
		0x00, /* only in UA1000_ROUTE_OUTPUT_DM_OFF */
	UA1000_SRC_MONITOR_OUT = 0x01,
	UA1000_SRC_INPUT_1_2 = 0x02,
	UA1000_SRC_INPUT_3_4,
	UA1000_SRC_INPUT_5_6,
	UA1000_SRC_INPUT_7_8,
	UA1000_SRC_INPUT_9_10,
	UA1000_SRC_WAVE_OUT_1_2 = 0x07,
	UA1000_SRC_WAVE_OUT_3_4,
	UA1000_SRC_WAVE_OUT_5_6,
	UA1000_SRC_WAVE_OUT_7_8,
	UA1000_SRC_WAVE_OUT_9_10,
};

bool ua1000_source_valid(enum ua1000_route_table table, unsigned int source);

struct ua1000_patchbay {
	uint8_t route[3][UA1000_PAIRS]; /* indexed by ua1000_route_table */
};

void ua1000_patchbay_init(struct ua1000_patchbay *patchbay);

/* The patch-bay presets of the vendor's panel, run by the device itself. */
enum ua1000_wave_in_preset {
	UA1000_WAVE_IN_KEEP = 0x00,
	UA1000_WAVE_IN_INITIAL = 0x03,
	UA1000_WAVE_IN_LOOP_BACK = 0x04,
};

enum ua1000_output_preset {
	UA1000_OUTPUT_KEEP = 0x00,
	UA1000_OUTPUT_INITIAL = 0x03,
	UA1000_OUTPUT_HARD_THRU = 0x04,
	UA1000_OUTPUT_MONITOR_OUT = 0x05,
};

/* What a preset leaves in the routing tables, as the device reports it. */
void ua1000_apply_wave_in_preset(struct ua1000_patchbay *patchbay,
				 enum ua1000_wave_in_preset preset);
void ua1000_apply_output_preset(struct ua1000_patchbay *patchbay,
				enum ua1000_output_preset preset);

/* Sections of a memory slot, as a bit set. */
enum ua1000_section {
	UA1000_SECTION_PANEL = 1 << 0,
	UA1000_SECTION_WAVE_IN = 1 << 1,
	UA1000_SECTION_OUTPUT = 1 << 2,
	UA1000_SECTION_MONITOR = 1 << 3,
	UA1000_SECTION_ALL = 0xF,
};

#define UA1000_SECTIONS 4

enum ua1000_memory_op {
	UA1000_MEMORY_STORE,
	UA1000_MEMORY_LOAD,
};

/*
 * Message builders.  Each returns 0, or -EINVAL when an argument is out of
 * range, in which case *msg is left with len == 0.
 *
 * Channels are numbered 0..9 and pairs 0..4.  Per-pair parameters (link and
 * solo) live at the pair's first channel.
 */
int ua1000_build_gain(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		      unsigned int channel, uint32_t gain);
int ua1000_build_pan(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		     unsigned int channel, uint16_t pan);
int ua1000_build_mute(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		      unsigned int channel, bool mute);
int ua1000_build_link(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		      unsigned int pair, bool link);
/* solo_mask: bit 0 is the pair's left channel, bit 1 its right channel. */
int ua1000_build_solo(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		      unsigned int pair, unsigned int solo_mask);

int ua1000_build_monitor_gain(struct ua1000_msg *msg, enum ua1000_side side,
			      uint32_t gain);
int ua1000_build_monitor_link(struct ua1000_msg *msg, bool link);

/*
 * The vendor's panel brackets every drag of a fader or pan knob: "begin"
 * before the first value and "end" after the last.  What the device does with
 * it is not known; see docs/PROTOCOL.md.
 */
int ua1000_build_edit_gesture(struct ua1000_msg *msg, bool begin);

int ua1000_build_panel(struct ua1000_msg *msg,
		       const struct ua1000_panel *panel);
int ua1000_build_user_set(struct ua1000_msg *msg,
			  const struct ua1000_panel *panel);
int ua1000_build_route(struct ua1000_msg *msg, enum ua1000_route_table table,
		       unsigned int pair, enum ua1000_source source);
int ua1000_build_preset(struct ua1000_msg *msg,
			enum ua1000_wave_in_preset wave_in,
			enum ua1000_output_preset output);
/* slot: 1..5 */
int ua1000_build_memory(struct ua1000_msg *msg, enum ua1000_memory_op op,
			unsigned int slot, unsigned int sections);

/*
 * A data request (RQ1) for block 02 bb 00 00, 256 bytes (bb 0..15); the
 * device answers with a DT1 of the block.  Block 00 is its current state.
 */
#define UA1000_BLOCK_CURRENT 0x00
int ua1000_build_block_request(struct ua1000_msg *msg, unsigned int block);

/* Roland checksum over the address and data bytes of a DT1 message. */
uint8_t ua1000_checksum(const uint8_t *bytes, size_t len);

/* A DT1 message as received, with its data still in the caller's buffer. */
struct ua1000_dt1 {
	uint8_t addr[4];
	const uint8_t *data;
	size_t len;
};

/*
 * Check that buf holds one well-formed DT1 message for this device: header,
 * terminator and checksum.  Returns 0 or -EBADMSG.
 */
int ua1000_parse_dt1(const uint8_t *buf, size_t len, struct ua1000_dt1 *dt1);

/* What a message from the device means. */
enum ua1000_event_type {
	UA1000_EVENT_UNKNOWN,
	UA1000_EVENT_PANEL, /* a front-panel switch changed */
	UA1000_EVENT_GLOBAL, /* full panel and patch-bay dump */
};

struct ua1000_event {
	enum ua1000_event_type type;
	struct ua1000_panel panel; /* PANEL and GLOBAL */
	struct ua1000_patchbay
		patchbay; /* GLOBAL: tables WAVE_IN, OUTPUT_DM_ON */
};

/*
 * Decode a DT1 message from the device.  The panel and patch bay in *event
 * must hold the current state on entry: a switch notification changes only
 * the switch it names, and a global dump does not carry the DM OFF table.
 */
void ua1000_decode(const struct ua1000_dt1 *dt1, struct ua1000_event *event);

/*
 * Reassemble SysEx messages from a MIDI byte stream that may split them
 * across reads.  Real-time bytes (0xF8..0xFF) may appear anywhere and are
 * skipped; a message longer than UA1000_SYSEX_MAX is dropped whole.
 */
struct ua1000_reader {
	size_t len;
	bool in_message;
	bool overflow;
	uint8_t buf[UA1000_SYSEX_MAX];
};

void ua1000_reader_init(struct ua1000_reader *reader);

/*
 * Feed one byte.  Returns true when it completes a message, which is then
 * available as reader->buf / reader->len until the next call.
 */
bool ua1000_reader_feed(struct ua1000_reader *reader, uint8_t byte);

#endif /* UA1000_PROTO_H */
