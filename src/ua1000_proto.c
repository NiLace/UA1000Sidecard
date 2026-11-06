/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000_proto.c - the UA-1000 control protocol, without any I/O
 *
 * This module owns the byte level of the protocol.  It turns mixer, patch-bay
 * and panel settings into complete Roland exclusive messages (DT1 data sets
 * and RQ1 data requests), checks and decodes the DT1 messages the device
 * sends back, and reassembles SysEx messages from a raw MIDI byte stream.  It
 * also holds the value laws every front end shares: dB and fader position to
 * the 23-bit linear gain, pan position to its raw value, rate code to Hz.
 *
 * Invariants: every message built here is a whole, checksummed SysEx that
 * fits in struct ua1000_msg, and a builder that rejects its arguments leaves
 * msg->len at 0, so a refused message is never sent half-built.  Values wider
 * than seven bits travel one nibble per byte, most significant first.
 *
 * It does not open, read or write a device (ua1000_midi.c does), keep the
 * device's state, nor decode the mixer part of the 256-byte block
 * (ua1000_state.c does both): the decoder here reads only the panel switches
 * and the patch-bay tables that notifications and global dumps carry.
 *
 * Used by ua1000_midi.c (stream reassembly and DT1 parsing), ua1000_state.c,
 * ua1000ctl.c and the GUI under src/gui/.
 *
 * See ua1000_proto.h for the interface and docs/PROTOCOL.md for where each
 * address and value comes from.
 */
#include "ua1000_proto.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/*
 * Framing of a Roland exclusive message: F0, manufacturer (Roland), device ID
 * (17, the factory default), the UA-1000's two-byte model ID, then the
 * command: DT1 (data set) or RQ1 (data request).
 */
#define SYSEX_START 0xF0
#define SYSEX_END 0xF7
#define ROLAND_ID 0x41
#define DEVICE_ID 0x10
#define MODEL_ID_HI 0x00
#define MODEL_ID_LO 0x69
#define CMD_DT1 0x12
#define CMD_RQ1 0x11

/* Header bytes before the address, F0 41 10 00 69 cmd: DT1 and RQ1 alike. */
#define DT1_HEADER 6

/*
 * Address 00 ss xx pp: the parameters.  ss names the section; xx is a
 * channel, a routing table, a memory slot or a MONITOR OUT side; pp is the
 * parameter, or the pair in the patch bay.
 */
#define AREA_MIXER 0x00
#define SECTION_SYSTEM 0x00
#define SECTION_NOTIFY 0x01
#define SECTION_PATCHBAY 0x02
#define SECTION_MONITOR_OUT 0x03
#define SECTION_INPUT_MIXER 0x04
#define SECTION_OUTPUT_MIXER 0x05

/* Last address byte of a channel parameter. */
#define PARAM_SOLO 0x00
#define PARAM_LINK 0x01
#define PARAM_MUTE 0x02
#define PARAM_PAN 0x03
#define PARAM_GAIN 0x07

/* Address byte 0x02 xx: the 256-byte blocks. */
#define AREA_BLOCKS 0x02
#define BLOCK_GLOBAL 0x00
#define BLOCK_USER_SET 0x02

/* SECTION_SYSTEM registers: 00 00 00 xx and 00 00 <slot + 2> xx. */
#define SYSTEM_EDIT_GESTURE 0x00
#define SYSTEM_PRESET 0x08
#define MEMORY_SLOT_BASE 0x02
#define MEMORY_STORE 0x04
#define MEMORY_LOAD 0x08
#define MEMORY_SECTION_STORE 0x02
#define MEMORY_SECTION_LOAD 0x01

/* SECTION_NOTIFY registers, sent by the device. */
#define NOTIFY_CLOCK 0x02
#define NOTIFY_OUTPUT_ALL 0x03
#define NOTIFY_MONO 0x04
#define NOTIFY_SOFT_CTRL 0x05

/*
 * Byte offsets inside the global block (02 00 00 00).  GLOBAL_DECODED_LEN is
 * how much of a dump ua1000_decode() reads: the panel bytes and the WAVE IN
 * and DM ON tables.
 */
#define GLOBAL_RATE 4
#define GLOBAL_CLOCK 5
#define GLOBAL_SWITCHES 6
#define GLOBAL_WAVE_IN 7
#define GLOBAL_OUTPUT_DM_ON (GLOBAL_WAVE_IN + UA1000_PAIRS)
#define GLOBAL_DECODED_LEN (GLOBAL_OUTPUT_DM_ON + UA1000_PAIRS)

/* The clock byte. */
#define CLOCK_SOURCE_MASK 0x03
#define CLOCK_DIGITAL_OUT_ADAT 0x04
#define CLOCK_FLAG 0x08

/* The switches byte. */
#define SWITCH_OUTPUT_ALL 0x01
#define SWITCH_MONO 0x02
#define SWITCH_SOFT_CTRL 0x04

/* ---- Value conversions ---- */

/*
 * dB to gain.  NaN and -INFINITY give silence; anything above +12.04 dB
 * saturates at UA1000_GAIN_MAX.
 */
uint32_t ua1000_gain_from_db(double db)
{
	if (isnan(db) || db == -INFINITY)
		return 0;

	double gain = round(UA1000_GAIN_UNITY * pow(10.0, db / 20.0));

	if (gain > UA1000_GAIN_MAX)
		return UA1000_GAIN_MAX;
	return (uint32_t)gain;
}

/* Gain to dB relative to unity; silence is -INFINITY. */
double ua1000_gain_to_db(uint32_t gain)
{
	if (gain == 0)
		return -INFINITY;
	return 20.0 * log10((double)gain / UA1000_GAIN_UNITY);
}

/*
 * Fader law, fitted to the 429 distinct gains of the captures:
 *
 *	gain(p) = FADER_OFFSET * (FADER_RATIO^p - 1)
 *
 * an exponential (0.0466 dB per position near the top) lowered by its value
 * at p = 0, so that the bottom of the fader is silence.  The two constants
 * are the exact solution of gain(766) = 0x200000 and gain(1022) = 0x7FFFFF;
 * every captured value lies within 1.3 of the formula.  See docs/PROTOCOL.md.
 */
#define FADER_RATIO 1.0053813222455683705
#define FADER_OFFSET 34946.583459509740866

/*
 * Fader position to gain by the law above; positions at or past
 * UA1000_FADER_MAX give the maximum.
 */
uint32_t ua1000_fader_to_gain(unsigned int position)
{
	if (position == 0)
		return 0;
	if (position >= UA1000_FADER_MAX)
		return UA1000_GAIN_MAX;

	double gain = round(FADER_OFFSET * (pow(FADER_RATIO, position) - 1.0));

	return gain > UA1000_GAIN_MAX ? UA1000_GAIN_MAX : (uint32_t)gain;
}

/*
 * The inverse of the fader law, rounded to the nearest position and clamped
 * to UA1000_FADER_MAX.
 */
unsigned int ua1000_gain_to_fader(uint32_t gain)
{
	if (gain == 0)
		return 0;

	double position =
		round(log1p((double)gain / FADER_OFFSET) / log(FADER_RATIO));

	if (position > UA1000_FADER_MAX)
		return UA1000_FADER_MAX;
	return (unsigned int)position;
}

/* Pan position to raw value; positions beyond +-20 are clamped. */
uint16_t ua1000_pan_from_position(int position)
{
	if (position < -UA1000_PAN_STEPS)
		position = -UA1000_PAN_STEPS;
	if (position > UA1000_PAN_STEPS)
		position = UA1000_PAN_STEPS;

	/* The vendor's panel rounds to the nearest step: 819.2 -> 819, 2457.6
	 * -> 2458. */
	unsigned int steps = (unsigned int)(position + UA1000_PAN_STEPS);
	unsigned int span = 2 * UA1000_PAN_STEPS;

	return (uint16_t)((steps * UA1000_PAN_RAW_MAX + span / 2) / span);
}

/*
 * Raw pan value to the nearest position; values past 0x8000 count as hard
 * right.
 */
int ua1000_pan_to_position(uint16_t raw)
{
	unsigned int span = 2 * UA1000_PAN_STEPS;

	if (raw > UA1000_PAN_RAW_MAX)
		raw = UA1000_PAN_RAW_MAX;
	return (int)((raw * span + UA1000_PAN_RAW_MAX / 2) /
		     UA1000_PAN_RAW_MAX) -
	       UA1000_PAN_STEPS;
}

/* ADAT works at 44.1 and 48 kHz only (see the header). */
bool ua1000_rate_allows_adat(enum ua1000_rate rate)
{
	return rate == UA1000_RATE_44100 || rate == UA1000_RATE_48000;
}

/*
 * Rate code to Hz, or 0 when the value is no rate code: callers use the 0 to
 * reject a code read from the device or found in a struct ua1000_panel.
 */
unsigned int ua1000_rate_hz(enum ua1000_rate rate)
{
	switch (rate) {
	case UA1000_RATE_44100:
		return 44100;
	case UA1000_RATE_48000:
		return 48000;
	case UA1000_RATE_88200:
		return 88200;
	case UA1000_RATE_96000:
		return 96000;
	}
	return 0;
}

/*
 * Hz to rate code.  False, with *rate untouched, for a rate the device does
 * not run at.
 */
bool ua1000_rate_from_hz(unsigned int hz, enum ua1000_rate *rate)
{
	static const enum ua1000_rate all[] = {
		UA1000_RATE_44100,
		UA1000_RATE_48000,
		UA1000_RATE_88200,
		UA1000_RATE_96000,
	};

	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		if (ua1000_rate_hz(all[i]) == hz) {
			*rate = all[i];
			return true;
		}
	}
	return false;
}

/* ---- Default state ---- */

/* The default panel switches; see the header. */
void ua1000_panel_init(struct ua1000_panel *panel)
{
	*panel = (struct ua1000_panel){
		.rate = UA1000_RATE_48000,
		.clock = UA1000_CLOCK_INTERNAL,
		.output_all = false, /* MAIN, the factory setting (Owner's
					Manual p. 44) */
		.soft_ctrl = true,
	};
}

/*
 * The default patch bay, what the two INITIAL presets leave: the inputs
 * recorded, the playback on the outputs, every DM OFF pair same as DM ON.
 */
void ua1000_patchbay_init(struct ua1000_patchbay *patchbay)
{
	ua1000_apply_wave_in_preset(patchbay, UA1000_WAVE_IN_INITIAL);
	ua1000_apply_output_preset(patchbay, UA1000_OUTPUT_INITIAL);
}

/*
 * Whether source is a code that table takes: 00 (same as DM ON) belongs to
 * the DM OFF table alone, and every table takes 01..0B.
 */
bool ua1000_source_valid(enum ua1000_route_table table, unsigned int source)
{
	if (source == UA1000_SRC_SAME_AS_DM_ON)
		return table == UA1000_ROUTE_OUTPUT_DM_OFF;
	return source <= UA1000_SRC_WAVE_OUT_9_10;
}

/*
 * INITIAL and LOOP BACK route WAVE IN pair p from INPUT or WAVE OUT pair p.
 * KEEP, or a value that is no preset, leaves the table alone.
 */
void ua1000_apply_wave_in_preset(struct ua1000_patchbay *patchbay,
				 enum ua1000_wave_in_preset preset)
{
	uint8_t *wave_in = patchbay->route[UA1000_ROUTE_WAVE_IN];
	uint8_t first;

	switch (preset) {
	case UA1000_WAVE_IN_INITIAL:
		first = UA1000_SRC_INPUT_1_2;
		break;
	case UA1000_WAVE_IN_LOOP_BACK:
		first = UA1000_SRC_WAVE_OUT_1_2;
		break;
	case UA1000_WAVE_IN_KEEP:
	default:
		return;
	}
	for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
		wave_in[pair] = (uint8_t)(first + pair);
}

/*
 * An OUTPUT preset rewrites the DM ON table and sets every DM OFF pair back
 * to same as DM ON.  That reset is inferred: the device's dump does not carry
 * the DM OFF table (docs/PROTOCOL.md).
 */
void ua1000_apply_output_preset(struct ua1000_patchbay *patchbay,
				enum ua1000_output_preset preset)
{
	uint8_t *dm_on = patchbay->route[UA1000_ROUTE_OUTPUT_DM_ON];
	uint8_t *dm_off = patchbay->route[UA1000_ROUTE_OUTPUT_DM_OFF];
	uint8_t first;

	switch (preset) {
	case UA1000_OUTPUT_INITIAL:
	case UA1000_OUTPUT_MONITOR_OUT:
		first = UA1000_SRC_WAVE_OUT_1_2;
		break;
	case UA1000_OUTPUT_HARD_THRU:
		first = UA1000_SRC_INPUT_1_2;
		break;
	case UA1000_OUTPUT_KEEP:
	default:
		return;
	}
	for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++) {
		dm_on[pair] = (uint8_t)(first + pair);
		dm_off[pair] = UA1000_SRC_SAME_AS_DM_ON;
	}
	/* MONITOR OUT replaces outputs 1-2 and 9-10 with the monitor mix. */
	if (preset == UA1000_OUTPUT_MONITOR_OUT) {
		dm_on[0] = UA1000_SRC_MONITOR_OUT;
		dm_on[UA1000_PAIRS - 1] = UA1000_SRC_MONITOR_OUT;
	}
}

/* ---- Message building ---- */

/* The value that brings the sum of the bytes to 0 modulo 128. */
uint8_t ua1000_checksum(const uint8_t *bytes, size_t len)
{
	unsigned int sum = 0;

	for (size_t i = 0; i < len; i++)
		sum += bytes[i];
	return (uint8_t)((128 - sum % 128) % 128);
}

/*
 * Lay out one Roland exclusive message: header, cmd, the address a0..a3, len
 * data bytes and the checksum over address and data.  Nothing checks len
 * here: every caller passes a fixed-size buffer of at most
 * UA1000_GLOBAL_BLOCK_SIZE bytes, which keeps the message within
 * UA1000_SYSEX_MAX.  Always returns 0, so a builder can return its result.
 */
static int build_message(struct ua1000_msg *msg, uint8_t cmd, uint8_t a0,
			 uint8_t a1, uint8_t a2, uint8_t a3,
			 const uint8_t *data, size_t len)
{
	uint8_t *p = msg->bytes;

	*p++ = SYSEX_START;
	*p++ = ROLAND_ID;
	*p++ = DEVICE_ID;
	*p++ = MODEL_ID_HI;
	*p++ = MODEL_ID_LO;
	*p++ = cmd;
	*p++ = a0;
	*p++ = a1;
	*p++ = a2;
	*p++ = a3;
	memcpy(p, data, len);
	p += len;
	*p = ua1000_checksum(msg->bytes + DT1_HEADER, 4 + len);
	p++;
	*p++ = SYSEX_END;
	msg->len = (size_t)(p - msg->bytes);
	return 0;
}

/* A DT1 (data set): what every builder but the block request sends. */
static int build_dt1(struct ua1000_msg *msg, uint8_t a0, uint8_t a1, uint8_t a2,
		     uint8_t a3, const uint8_t *data, size_t len)
{
	return build_message(msg, CMD_DT1, a0, a1, a2, a3, data, len);
}

/* An RQ1 for the 256 bytes of block 02 bb 00 00, bb = block. */
int ua1000_build_block_request(struct ua1000_msg *msg, unsigned int block)
{
	/* the size, 256, in 7-bit bytes */
	static const uint8_t size[4] = { 0x00, 0x00, 0x02, 0x00 };

	if (block > 0x0F) {
		msg->len = 0;
		return -EINVAL;
	}
	return build_message(msg, CMD_RQ1, AREA_BLOCKS, (uint8_t)block, 0x00,
			     0x00, size, sizeof(size));
}

/* Refuse a builder's arguments: empty the message, return -EINVAL. */
static int invalid(struct ua1000_msg *msg)
{
	msg->len = 0;
	return -EINVAL;
}

/* A mixer's address section; the caller has checked the mixer. */
static uint8_t mixer_section(enum ua1000_mixer mixer)
{
	return mixer == UA1000_MIXER_INPUT ? SECTION_INPUT_MIXER
					   : SECTION_OUTPUT_MIXER;
}

/* True for the two mixers, false for any other value in the enum. */
bool ua1000_mixer_valid(enum ua1000_mixer mixer)
{
	return mixer == UA1000_MIXER_INPUT || mixer == UA1000_MIXER_OUTPUT;
}

/*
 * Values wider than seven bits travel as one nibble per byte, MSB first:
 * write the low count nibbles of value to out[0..count-1].
 */
static void nibbles(uint8_t *out, uint32_t value, unsigned int count)
{
	for (unsigned int i = 0; i < count; i++)
		out[i] = (uint8_t)((value >> (4 * (count - 1 - i))) & 0x0F);
}

/* Data length of a gain (23 bits) and of a pan value (16 bits). */
#define GAIN_NIBBLES 6
#define PAN_NIBBLES 4

/*
 * A one-byte channel parameter of a mixer: mute, link or solo.  Link and
 * solo pass the pair's first channel, where the device keeps them.
 */
static int build_channel_flag(struct ua1000_msg *msg, enum ua1000_mixer mixer,
			      unsigned int channel, uint8_t param,
			      uint8_t value)
{
	if (!ua1000_mixer_valid(mixer) || channel >= UA1000_CHANNELS)
		return invalid(msg);
	return build_dt1(msg, AREA_MIXER, mixer_section(mixer),
			 (uint8_t)channel, param, &value, 1);
}

/* Channel gain: six nibbles at 00 m cc 07. */
int ua1000_build_gain(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		      unsigned int channel, uint32_t gain)
{
	uint8_t data[GAIN_NIBBLES];

	if (!ua1000_mixer_valid(mixer) || channel >= UA1000_CHANNELS ||
	    gain > UA1000_GAIN_MAX)
		return invalid(msg);
	nibbles(data, gain, GAIN_NIBBLES);
	return build_dt1(msg, AREA_MIXER, mixer_section(mixer),
			 (uint8_t)channel, PARAM_GAIN, data, sizeof(data));
}

/* Channel pan: four nibbles at 00 m cc 03. */
int ua1000_build_pan(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		     unsigned int channel, uint16_t pan)
{
	uint8_t data[PAN_NIBBLES];

	if (!ua1000_mixer_valid(mixer) || channel >= UA1000_CHANNELS ||
	    pan > UA1000_PAN_RAW_MAX)
		return invalid(msg);
	nibbles(data, pan, PAN_NIBBLES);
	return build_dt1(msg, AREA_MIXER, mixer_section(mixer),
			 (uint8_t)channel, PARAM_PAN, data, sizeof(data));
}

/* Channel on/off at 00 m cc 02: 01 is off (muted), 00 on. */
int ua1000_build_mute(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		      unsigned int channel, bool mute)
{
	return build_channel_flag(msg, mixer, channel, PARAM_MUTE, mute);
}

/* Pair link, at the pair's first channel. */
int ua1000_build_link(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		      unsigned int pair, bool link)
{
	if (pair >= UA1000_PAIRS)
		return invalid(msg);
	return build_channel_flag(msg, mixer, 2 * pair, PARAM_LINK, link);
}

/*
 * Pair solo, at the pair's first channel.  The value says which channel is
 * soloed, and 3 silences everything (docs/PROTOCOL.md, Solo).
 */
int ua1000_build_solo(struct ua1000_msg *msg, enum ua1000_mixer mixer,
		      unsigned int pair, unsigned int solo_mask)
{
	if (pair >= UA1000_PAIRS || solo_mask > 3)
		return invalid(msg);
	return build_channel_flag(msg, mixer, 2 * pair, PARAM_SOLO,
				  (uint8_t)solo_mask);
}

/* MONITOR OUT gain of one side, at 00 03 0s 07. */
int ua1000_build_monitor_gain(struct ua1000_msg *msg, enum ua1000_side side,
			      uint32_t gain)
{
	uint8_t data[GAIN_NIBBLES];

	if ((side != UA1000_LEFT && side != UA1000_RIGHT) ||
	    gain > UA1000_GAIN_MAX)
		return invalid(msg);
	nibbles(data, gain, GAIN_NIBBLES);
	return build_dt1(msg, AREA_MIXER, SECTION_MONITOR_OUT, (uint8_t)side,
			 PARAM_GAIN, data, sizeof(data));
}

/* MONITOR OUT link, at 00 03 00 01: the left side's address. */
int ua1000_build_monitor_link(struct ua1000_msg *msg, bool link)
{
	uint8_t value = link;

	return build_dt1(msg, AREA_MIXER, SECTION_MONITOR_OUT, UA1000_LEFT,
			 PARAM_LINK, &value, 1);
}

/* The drag bracket at 00 00 00 00: 00 opens it, 01 closes it. */
int ua1000_build_edit_gesture(struct ua1000_msg *msg, bool begin)
{
	uint8_t value = begin ? 0x00 : 0x01;

	return build_dt1(msg, AREA_MIXER, SECTION_SYSTEM, 0x00,
			 SYSTEM_EDIT_GESTURE, &value, 1);
}

/*
 * A rate and a clock source the device knows: the panel and USER SET
 * builders refuse anything else.
 */
static bool panel_valid(const struct ua1000_panel *panel)
{
	return ua1000_rate_hz(panel->rate) != 0 &&
	       (panel->clock == UA1000_CLOCK_INTERNAL ||
		panel->clock == UA1000_CLOCK_EXT_SPDIF ||
		panel->clock == UA1000_CLOCK_EXT_ADAT);
}

/*
 * The clock byte: source in bits 0-1, ADAT output in bit 2.  Bit 3
 * (CLOCK_FLAG) is set by the device only, never written.
 */
static uint8_t clock_byte(const struct ua1000_panel *panel)
{
	return (uint8_t)(panel->clock |
			 (panel->digital_out_adat ? CLOCK_DIGITAL_OUT_ADAT
						  : 0));
}

/* The switches byte: OUTPUT ALL, MONO and SOFT CTRL in bits 0-2. */
static uint8_t switches_byte(const struct ua1000_panel *panel)
{
	return (uint8_t)((panel->output_all ? SWITCH_OUTPUT_ALL : 0) |
			 (panel->mono ? SWITCH_MONO : 0) |
			 (panel->soft_ctrl ? SWITCH_SOFT_CTRL : 0));
}

/*
 * The panel switches are written as the whole 256-byte block, laid out
 * exactly as the vendor's panel sends it: bytes 1..3 and 7..255 are zero even
 * though the device reports the patch bay at 7..16 when it dumps the block.
 */
int ua1000_build_panel(struct ua1000_msg *msg, const struct ua1000_panel *panel)
{
	uint8_t data[UA1000_GLOBAL_BLOCK_SIZE] = { 0 };

	if (!panel_valid(panel))
		return invalid(msg);
	data[0] = 0x01;
	data[GLOBAL_RATE] = (uint8_t)panel->rate;
	data[GLOBAL_CLOCK] = clock_byte(panel);
	data[GLOBAL_SWITCHES] = switches_byte(panel);
	return build_dt1(msg, AREA_BLOCKS, BLOCK_GLOBAL, 0x00, 0x00, data,
			 sizeof(data));
}

/*
 * The USER SET, laid out as the vendor's panel writes it: 01 02 02 02, then
 * the rate, clock and switches bytes, and zeros.
 */
int ua1000_build_user_set(struct ua1000_msg *msg,
			  const struct ua1000_panel *panel)
{
	uint8_t data[UA1000_GLOBAL_BLOCK_SIZE] = { 0 };

	if (!panel_valid(panel))
		return invalid(msg);
	data[0] = 0x01;
	data[1] = 0x02;
	data[2] = 0x02;
	data[3] = 0x02;
	data[GLOBAL_RATE] = (uint8_t)panel->rate;
	data[GLOBAL_CLOCK] = clock_byte(panel);
	data[GLOBAL_SWITCHES] = switches_byte(panel);
	return build_dt1(msg, AREA_BLOCKS, BLOCK_USER_SET, 0x00, 0x00, data,
			 sizeof(data));
}

/* One patch-bay entry, at 00 02 tt pp: table tt, pair pp. */
int ua1000_build_route(struct ua1000_msg *msg, enum ua1000_route_table table,
		       unsigned int pair, enum ua1000_source source)
{
	uint8_t value = (uint8_t)source;

	if (table > UA1000_ROUTE_OUTPUT_DM_OFF || pair >= UA1000_PAIRS ||
	    !ua1000_source_valid(table, source))
		return invalid(msg);
	return build_dt1(msg, AREA_MIXER, SECTION_PATCHBAY, (uint8_t)table,
			 (uint8_t)pair, &value, 1);
}

/*
 * Run patch-bay presets on the device, data 00 w o 00 at 00 00 00 08; KEEP
 * (00) leaves that table unchanged.
 */
int ua1000_build_preset(struct ua1000_msg *msg,
			enum ua1000_wave_in_preset wave_in,
			enum ua1000_output_preset output)
{
	uint8_t data[4] = { 0x00, (uint8_t)wave_in, (uint8_t)output, 0x00 };
	bool wave_in_ok = wave_in == UA1000_WAVE_IN_KEEP ||
			  wave_in == UA1000_WAVE_IN_INITIAL ||
			  wave_in == UA1000_WAVE_IN_LOOP_BACK;
	bool output_ok = output == UA1000_OUTPUT_KEEP ||
			 output == UA1000_OUTPUT_INITIAL ||
			 output == UA1000_OUTPUT_HARD_THRU ||
			 output == UA1000_OUTPUT_MONITOR_OUT;

	if (!wave_in_ok || !output_ok)
		return invalid(msg);
	return build_dt1(msg, AREA_MIXER, SECTION_SYSTEM, 0x00, SYSTEM_PRESET,
			 data, sizeof(data));
}

/*
 * Store or load a memory slot at 00 00 (slot + 2) op.  Data byte i includes
 * section bit i (enum ua1000_section order) with 02 for a store or 01 for a
 * load, and skips it with 00.
 */
int ua1000_build_memory(struct ua1000_msg *msg, enum ua1000_memory_op op,
			unsigned int slot, unsigned int sections)
{
	uint8_t mark = op == UA1000_MEMORY_STORE ? MEMORY_SECTION_STORE
						 : MEMORY_SECTION_LOAD;
	uint8_t data[4];

	if ((op != UA1000_MEMORY_STORE && op != UA1000_MEMORY_LOAD) ||
	    slot < 1 || slot > UA1000_MEMORY_SLOTS || sections == 0 ||
	    sections > UA1000_SECTION_ALL)
		return invalid(msg);
	for (unsigned int i = 0; i < 4; i++)
		data[i] = (sections & (1u << i)) ? mark : 0x00;
	return build_dt1(msg, AREA_MIXER, SECTION_SYSTEM,
			 (uint8_t)(MEMORY_SLOT_BASE + slot),
			 op == UA1000_MEMORY_STORE ? MEMORY_STORE : MEMORY_LOAD,
			 data, sizeof(data));
}

/* ---- Parsing ---- */

/* Check a DT1 from the device and point *dt1 into buf; see the header. */
int ua1000_parse_dt1(const uint8_t *buf, size_t len, struct ua1000_dt1 *dt1)
{
	static const uint8_t header[DT1_HEADER] = {
		SYSEX_START, ROLAND_ID,	  DEVICE_ID,
		MODEL_ID_HI, MODEL_ID_LO, CMD_DT1,
	};

	/* The shortest DT1 carries one data byte. */
	if (len < UA1000_DT1_OVERHEAD + 1 || len > UA1000_SYSEX_MAX)
		return -EBADMSG;
	if (memcmp(buf, header, sizeof(header)) != 0 ||
	    buf[len - 1] != SYSEX_END)
		return -EBADMSG;
	/* Between F0 and F7 every byte is a 7-bit data byte. */
	for (size_t i = 1; i < len - 1; i++)
		if (buf[i] & 0x80)
			return -EBADMSG;

	size_t body = len - DT1_HEADER - 2; /* address and data */

	if (ua1000_checksum(buf + DT1_HEADER, body) != buf[len - 2])
		return -EBADMSG;
	memcpy(dt1->addr, buf + DT1_HEADER, 4);
	dt1->data = buf + DT1_HEADER + 4;
	dt1->len = body - 4;
	return 0;
}

/*
 * Decode a clock byte into panel: the source, the digital output and the
 * flag the device sets with an external clock.  Notifications and global
 * dumps carry the same byte.
 */
static void decode_clock(uint8_t value, struct ua1000_panel *panel)
{
	unsigned int source = value & CLOCK_SOURCE_MASK;

	/* Source 3 has never been seen; keep the current one if it shows up. */
	if (source <= UA1000_CLOCK_EXT_ADAT)
		panel->clock = (enum ua1000_clock)source;
	panel->digital_out_adat = value & CLOCK_DIGITAL_OUT_ADAT;
	panel->clock_flag = value & CLOCK_FLAG;
}

/* Decode the switches byte of a global dump into panel. */
static void decode_switches(uint8_t value, struct ua1000_panel *panel)
{
	panel->output_all = value & SWITCH_OUTPUT_ALL;
	panel->mono = value & SWITCH_MONO;
	panel->soft_ctrl = value & SWITCH_SOFT_CTRL;
}

/*
 * A panel-switch notification, 00 01 00 nn with one data byte: change the
 * switch that nn names, and nothing else.  False, panel untouched, for an nn
 * not known.
 */
static bool decode_notification(const struct ua1000_dt1 *dt1,
				struct ua1000_panel *panel)
{
	uint8_t value = dt1->data[0];

	switch (dt1->addr[3]) {
	case NOTIFY_CLOCK:
		decode_clock(value, panel);
		return true;
	case NOTIFY_OUTPUT_ALL:
		panel->output_all = value;
		return true;
	case NOTIFY_MONO:
		panel->mono = value;
		return true;
	case NOTIFY_SOFT_CTRL:
		panel->soft_ctrl = value;
		return true;
	default:
		return false;
	}
}

/*
 * A global dump: the panel switches and the WAVE IN and DM ON tables.  A byte
 * that is not a valid code leaves the current value.  In a block read with
 * RQ1, byte 4 is the SAMPLE RATE switch's position, a rate code only on the
 * USER SET position (docs/PROTOCOL.md): the other positions are ignored.
 */
static void decode_global(const struct ua1000_dt1 *dt1,
			  struct ua1000_event *event)
{
	const uint8_t *data = dt1->data;
	enum ua1000_rate rate = (enum ua1000_rate)data[GLOBAL_RATE];

	if (ua1000_rate_hz(rate) != 0)
		event->panel.rate = rate;
	decode_clock(data[GLOBAL_CLOCK], &event->panel);
	decode_switches(data[GLOBAL_SWITCHES], &event->panel);
	for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++) {
		uint8_t wave_in = data[GLOBAL_WAVE_IN + pair];
		uint8_t dm_on = data[GLOBAL_OUTPUT_DM_ON + pair];

		if (ua1000_source_valid(UA1000_ROUTE_WAVE_IN, wave_in))
			event->patchbay.route[UA1000_ROUTE_WAVE_IN][pair] =
				wave_in;
		if (ua1000_source_valid(UA1000_ROUTE_OUTPUT_DM_ON, dm_on))
			event->patchbay.route[UA1000_ROUTE_OUTPUT_DM_ON][pair] =
				dm_on;
	}
}

/* Classify a DT1 from the device and decode it; see the header. */
void ua1000_decode(const struct ua1000_dt1 *dt1, struct ua1000_event *event)
{
	const uint8_t *a = dt1->addr;

	event->type = UA1000_EVENT_UNKNOWN;
	if (a[0] == AREA_MIXER && a[1] == SECTION_NOTIFY && a[2] == 0x00 &&
	    dt1->len == 1) {
		if (decode_notification(dt1, &event->panel))
			event->type = UA1000_EVENT_PANEL;
		return;
	}
	if (a[0] == AREA_BLOCKS && a[1] == BLOCK_GLOBAL && a[2] == 0x00 &&
	    a[3] == 0x00 && dt1->len >= GLOBAL_DECODED_LEN) {
		decode_global(dt1, event);
		event->type = UA1000_EVENT_GLOBAL;
	}
}

/* ---- Stream reassembly ---- */

/* No message in progress. */
void ua1000_reader_init(struct ua1000_reader *reader)
{
	reader->len = 0;
	reader->in_message = false;
	reader->overflow = false;
}

/*
 * Feed one byte; see the header.  A message runs from F0 to F7: an F0 in the
 * middle starts it again, any other status byte abandons it, and one too long
 * for the buffer is read to its F7 and then dropped.
 */
bool ua1000_reader_feed(struct ua1000_reader *reader, uint8_t byte)
{
	if (byte >= 0xF8) /* real-time messages may interleave anything */
		return false;
	if (byte == SYSEX_START) {
		reader->len = 0;
		reader->in_message = true;
		reader->overflow = false;
	} else if (!reader->in_message) {
		return false;
	} else if (byte & 0x80 && byte != SYSEX_END) {
		/* Any other status byte aborts the message. */
		reader->in_message = false;
		return false;
	}

	if (reader->len < sizeof(reader->buf))
		reader->buf[reader->len++] = byte;
	else
		reader->overflow = true;

	if (byte != SYSEX_END)
		return false;
	reader->in_message = false;
	return !reader->overflow;
}
