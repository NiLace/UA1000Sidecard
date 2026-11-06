# The UA-1000 control protocol

How a computer controls the mixer, patch bay and panel switches of a Roland/EDIROL UA-1000, as far as it is
known, and where each piece of knowledge comes from. `src/ua1000_proto.c` implements exactly this document.

> Files named below under `research/` and `tests/` (the USB captures, the probes, the tests that replay them) belong to the project's development repository and are not published here.

**Provenance tags.** Every fact below carries one:

| Tag | Meaning |
|---|---|
| **captured** | Seen in the USB traffic of the vendor's Windows control panel (`research/usb-captures/`), and reproduced byte for byte by `tests/test_captures.c`. |
| **device** | Observed on the device itself with the first-generation tools of this project, without a capture in this repository. |
| **inferred** | Follows from a pattern in captured data, but that exact value was never seen. |
| **unknown** | Seen, but its meaning has not been established. |

No vendor protocol documentation was used: none is published. The device's own rules and defaults (which rates
take ADAT, how its front panel behaves) are cited from its Owner's Manual where they are used.

## Transport

The UA-1000 (USB ID `0582:0044`) is driven by the Linux `snd-ua101` driver. Its fourth interface is a USB-MIDI
port (bulk endpoints 4 OUT and 3 IN) with two cables: **cable 0 is the MIDI IN/OUT jacks, cable 1 carries the
control messages**. The captures show cable 1 on every control packet in both directions (**captured**). The
kernel's usbmidi port table names the two "UA-1000 MIDI" and "UA-1000 Control", and the `snd-usb-audio` quirk
that drove the device before `snd-ua101` declared both cables; `snd-ua101` declares cable 0 only, so its single
port reaches the jacks and nothing reaches the mixer. `kernel/` has the patch that declares both
cables again: the jacks stay `hw:N,0,0`, and the control port becomes `hw:N,0,1`, which the editor finds by
its name.

All messages are Roland exclusive messages:

```
F0 41 10 00 69 cmd a0 a1 a2 a3 data... sum F7
```

| Byte | Value |
|---|---|
| `41` | Roland |
| `10` | device ID (17), the factory default |
| `00 69` | model ID of the UA-1000 |
| `cmd` | `12` = DT1, data set (both directions); `11` = RQ1, data request (see [Reading the device](#reading-the-device-rq1)) |
| `a0..a3` | address |
| `sum` | Roland checksum: `(128 − (Σ address and data bytes) mod 128) mod 128` |

Values wider than seven bits travel as **one nibble per byte**, most significant first.

## Address map

`m` is the mixer: `04` = INPUT MONITOR (the inputs), `05` = OUTPUT MONITOR (the computer's playback).
`cc` is a channel, `00`..`09`. Per-pair parameters live at the pair's first channel (`00`, `02`, … `08`).

| Address | Data | Meaning | Provenance |
|---|---|---|---|
| `00 m cc 07` | 6 nibbles | channel gain, see *Gain* | captured: input 1, 2, 8; output 1, 2, 9, 10. Other channels inferred. |
| `00 m cc 03` | 4 nibbles | channel pan, see *Pan* | captured: input 1, 2; output 1 |
| `00 m cc 02` | `00` on, `01` off | channel on/off | captured: input 1, 2; output 1, 2 |
| `00 m cc 01` | `00`/`01` | link the pair (at its first channel) | captured: input pair 1-2; output pair 1-2 |
| `00 m cc 00` | `01` left, `02` right (linked: either = the pair) | solo, ONE per pair; `03` silences everything | captured: `01`, `02`, `00`; device: see *Solo* |
| `00 03 0s 07` | 6 nibbles | MONITOR OUT gain, `s` = `00` left, `01` right | captured |
| `00 03 00 01` | `00`/`01` | MONITOR OUT link | captured |
| `00 02 tt pp` | source | patch bay: table `tt`, stereo pair `pp` (`00`..`04`) | captured: tables 0, 1, 2 |
| `00 00 00 08` | 4 bytes | patch-bay preset | captured |
| `00 00 ss 04` | 4 bytes | store memory slot `ss − 2` | captured: slots 1, 2, 5 |
| `00 00 ss 08` | 4 bytes | load memory slot `ss − 2` | captured: slots 1, 2, 5 |
| `00 00 00 00` | `00` / `01` | brackets a fader or pan drag | captured; effect unknown |
| `02 00 00 00` | 256 bytes | the panel block | captured |
| `02 02 00 00` | 256 bytes | the USER SET (power-on settings: the whole state, loaded at power-on on the USER SET position) | captured, first 68 data bytes only; read whole with RQ1 (device) |
| `00 01 00 nn` | 1 byte | sent by the device: a panel switch changed | see *Notifications* |

### Gain

A 23-bit linear amplitude: `0` is silence, **`0x200000` is unity (0 dB)** and `0x7FFFFF` is the maximum,
+12.04 dB (**captured**: a capture that ends with the vendor's fader back at 0 sends exactly `0x200000`, and
`0x7FFFFF` is the largest value sent).

The vendor's faders have **1023 positions**. Fitted to the 429 distinct non-zero gains in the captures (the 430th is 0, silence):

```
gain(p) = 34946.583 × (1.0053813^p − 1),   p = 0 .. 1022
```

an exponential of 0.0466 dB per position, lowered by its value at `p = 0` so that the bottom of the fader is
silence. The two constants are the exact solution of `gain(766) = 0x200000` and `gain(1022) = 0x7FFFFF`. Every
captured value lies within 1.3 of the formula, and each falls on its own position (**captured**, checked by
`tests/test_captures.c`). The last bit differs because the vendor rounds differently. That is −138 dB from full
scale, so it is not pursued.

The editor's faders move through the same 1023 positions; `ua1000ctl` takes any value in dB.

### Pan

41 positions, from hard left `0x0000` through centre `0x4000` to hard right `0x8000`. Position `k` (0..40) is
`k × 0x8000 / 40` rounded to the nearest integer: position 3 is 2457.6, sent as 2458 (**captured**: 1190 pan
messages, every one a position).

### Linked pairs

The device stores the link flag; the editor makes the pair follow. When a pair is linked, the vendor's panel:

- sends every gain change to both channels (**captured**);
- sends on/off to both channels (**captured**);
- sends solo as `02` for the pair, whichever strip is soloed (**captured**, one capture). The first-generation
  GUI sent `03`.

### Solo

The pair's solo register holds **which** channel is soloed, not a bit per channel (**device**, 5-Nov-2026, music on
OUTPUT 1-2 panned hard left and right, each value sent alone):

| Value | Pair unlinked | Pair linked |
|---|---|---|
| `00` | no solo: everything heard | no solo |
| `01` | the left channel alone | the pair |
| `02` | the right channel alone | the pair |
| `03` | **silence**: nothing heard | **silence** |

So an unlinked pair can solo one channel at a time, and `03` is never sent: soloing one channel of an unlinked pair
takes the solo from its partner. Through a link change the editor keeps the strips showing what is heard: linking
a pair with one channel soloed solos the pair, and unlinking a soloed pair takes the solo off both.

What the vendor's panel does with pan on a linked pair was not captured. This editor mirrors it, so the partner gets the opposite
position (**inferred**, to be checked on the device).

### Patch bay

Tables: `00` WAVE IN (what the computer records), `01` OUTPUT with direct monitor on, `02` OUTPUT with every
direct monitor off.

| Source | Code |
|---|---|
| same as the DM ON table (table `02` only) | `00` |
| MONITOR OUT | `01` |
| INPUT 1-2 … 9-10 | `02` … `06` |
| WAVE OUT 1-2 … 9-10 | `07` … `0B` |

A WAVE OUT pair as a WAVE IN source is that pair **after** the OUTPUT MONITOR mixer's fader, mute and pan
(**device**, 5-Nov-2026: WAVE IN 1-2 = WAVE OUT 1-2, music playing at 0.47-0.51 peak: captured 0.122 on both sides
with the pair panned hard left and right, the same as MAIN; silence with the pair muted while the playback went on;
equal left and right, 0.262, with the pair centred). Whether the INPUT MONITOR's channels enter it is unknown: no
input carried a signal.

Presets, address `00 00 00 08`, data `00 w o 00`:

| `w` (WAVE IN) | `o` (OUTPUT) | Result, as the device reports it afterwards |
|---|---|---|
| `03` initial | | WAVE IN = INPUT 1-2 … 9-10 |
| `04` loop back | | WAVE IN = WAVE OUT 1-2 … 9-10 |
| | `03` initial | DM ON = WAVE OUT 1-2 … 9-10 |
| | `04` hard thru | DM ON = INPUT 1-2 … 9-10 |
| | `05` monitor out | DM ON = MONITOR OUT, WAVE OUT 3-4, 5-6, 7-8, MONITOR OUT |

`00` leaves a table unchanged. The results are **captured**: after each preset button the device dumps the panel
block, which carries the WAVE IN and DM ON tables (`tests/test_captures.c` matches all five). The dump does not
carry the DM OFF table. That the OUTPUT presets reset it to "same" is **inferred**.

### Memory

Five slots. Address `00 00 (slot + 2) op`, `op` = `04` store or `08` load. The four data bytes select the
sections: panel switches, WAVE IN, OUTPUT, MONITOR (the mixer), in that order, each `02` (store) or `01` (load) to
include it, `00` to skip it (**captured**).

After a load of slots 1 and 2 the device dumped the panel block; after a load of slot 5 it sent nothing
(**captured**). That it dumps only when the load changes the panel or patch bay is **inferred** (the first-generation
GUI observed the same on the device). Those dumps carry no mixer settings, but the device gives them when asked
([Reading the device](#reading-the-device-rq1)).

### The panel block

Address `02 00 00 00`, 256 data bytes. The device ignores smaller writes to the switches inside it (**device**),
so the vendor's panel always writes the whole block (**captured**: two complete writes in the pcapng capture):

| Byte | Written by the panel | Dumped by the device |
|---|---|---|
| 0 | `01` | `01` |
| 1–3 | `00` | `01 01 01` |
| 4 | sample rate, always `09` in the captures | sample rate |
| 5 | clock byte | clock byte |
| 6 | switches byte | switches byte |
| 7–11 | `00` | WAVE IN table |
| 12–16 | `00` | OUTPUT DM ON table |
| 17–255 | `00` | `00` after a memory load or a preset (**captured**); the rest of the device's state when asked with RQ1 (below) |

The editor's default panel is the factory one (Owner's Manual, p. 44): 48 kHz, internal clock, S/PDIF output,
OUTPUT VOLUME on MAIN, SOFT CTRL on. Every dump in the captures showed ALL instead (**captured**: the state the
captured device was in).

Sample-rate codes: `09` = 48 kHz, `0B` = 96 kHz (**captured**); `08` = 44.1 kHz, `0A` = 88.2 kHz (**inferred**
from the sequence).

Clock byte: bits 0–1 = source (`0` internal, `1` external S/PDIF or word clock, `2` external ADAT or word clock),
bit 2 = digital output ADAT (else S/PDIF) (**captured**). The device also sets bit 3 in its notifications whenever
an external source is selected (**unknown**: lock, or merely "external"). The vendor's panel selects ADAT output
together with the ADAT clock (**captured**: it writes `06`).

Switches byte: bit 0 = OUTPUT VOLUME knob acts on ALL outputs (else MAIN), bit 1 = MONO, bit 2 = SOFT CTRL
(direct monitor follows the software mixer) (**captured**).

The editor writes the block exactly as the vendor's panel does: zeros where the device dumps the patch bay. With
byte 4, it writes the rate the driver reports. The captures do not show whether the device honours byte 4 on a
write. The editor never changes it, and the rate is chosen only in the USER SET.

### The USER SET

Address `02 02 00 00`, 256 data bytes: `01 02 02 02`, then rate, clock byte and switches byte as in the panel
block (**captured**: the export kept only the first 68 data bytes, which are `00` after byte 6; the rest is
**inferred** to be `00` too). Asked with RQ1, the device answers the whole block with the layout of the panel block
read the same way, starting `01 01 01 01` (**device**); on the unit measured it was byte for byte the panel block
(**device**, 6-Nov-2026), so whether it holds a separate USER SET or the running settings is **unknown**.

### Notifications

When a panel switch changes on the device, it sends `00 01 00 nn` with one data byte:

| `nn` | Data | Provenance |
|---|---|---|
| `02` | the clock byte | captured |
| `03` | OUTPUT VOLUME: `00` MAIN, `01` ALL | device |
| `04` | MONO | device |
| `05` | SOFT CTRL | device |

Each was seen on the device (5 and 6-Nov-2026): pressing MONO, SOFT CTRL and the output volume select switch (ALL)
on the front panel moved the editor's switch with it. ALL changes on a short press; the Owner's Manual (p. 14) asks
for two seconds, which the device did not need.

### The drag bracket

Around every drag of a fader or a pan knob, the vendor's panel writes `00` to `00 00 00 00` before the first value
and `01` after the last one: 46 drags in the captures, every one bracketed (**captured**). Single clicks on
buttons are not bracketed. What the device does with the bracket is **unknown**. The GUI sends it the same way,
closing it 250 ms after the control stops moving; `ua1000ctl` does not send it.

## Reading the device (RQ1)

Measured on the device on 6 November 2026; the probe, the dumps and the experiment are in `research/rq1/`.

A data request is `F0 41 10 00 69 11 a0 a1 a2 a3 s0 s1 s2 s3 sum F7`: an address and a size, both in 7-bit bytes,
and the Roland checksum over both. The vendor's panel sent one (`research/sysex/rq1_monitor_vol_l.syx`, address
`00 03 00 07`, size 6), and the device does not answer that one.

- **Only the blocks answer** (**device**). Of every address `a0 a1 00 00` with `a0` 0–15 and `a1` 0–31, asked for
  256 bytes, only `02 00` … `02 0F` answer, each with a DT1 of 256 data bytes from its own address. `02 10` …
  `02 1F` answer too, with the address of `02 00` … `02 0F`; the content of `02 10` differs from `02 00`'s
  (**unknown**). No parameter address (`00 m cc pp`) answers.
- **Block `02 00` is the device's current state, mixer included** (**device**): every single edit below changed the
  bytes listed, and nothing else, and undoing them all gave back the block byte for byte.

| Bytes | Content | How it was found |
|---|---|---|
| 0–3 | `01 01 01 01` | always |
| 4 | sample rate | as the panel block |
| 5 | clock byte | as the panel block |
| 6 | switches byte | as the panel block |
| 7–11 | WAVE IN table, one source per pair | as the panel block |
| 12–16 | OUTPUT DM ON table | as the panel block |
| 17–21 | OUTPUT DM OFF table (`00` = same as DM ON) | **inferred**: five bytes, all `00` with every pair on "same" |
| 22 | INPUT solo: the value of the LAST solo written to the input mixer (`01`, `02`, `00`), whatever its pair | each channel soloed alone gave its side's value, the same for all five pairs; with pairs 1-2 and 3-4 both soloed, turning 3-4 off gave `00` (**device**) |
| 23 | OUTPUT solo, the same | as byte 22 (**device**) |
| 24 | MONITOR OUT link, `01` linked | edit |
| 25–26 | INPUT links, a bit per pair, four pairs a byte (25: pairs 1-2 … 7-8 in bits 0–3; 26: pair 9-10) | edits on pairs 3-4 (`02` in 25) and the state found (`01` in 26) |
| 27–28 | OUTPUT links, the same way | edits on pairs 1-2 (bit 0 of 27) and 9-10 (bit 0 of 28) |
| 29 | **unknown** (`00`) | |
| 30–32 | INPUT mutes ("off"), a bit per channel, four channels a byte (30: 1–4; 31: 5–8; 32: 9–10) | edits on 3 (`04` in 30), 9 and 10 (`03` in 32) |
| 33–35 | OUTPUT mutes, the same way | edit on 1 and 2 (`03` in 33) |
| 36–75 | INPUT pans, 4 nibbles each as the pan message, channel `c` at 36 + 4c | edit on 3 (44–47) |
| 76–115 | OUTPUT pans, the same, at 76 + 4c | the state found (pairs hard left, hard right) |
| 116–127 | MONITOR OUT gains, left then right, 6 nibbles each as the gain message | edit |
| 128–187 | INPUT gains, channel `c` at 128 + 6c | edit on 3 (140–145) |
| 188–247 | OUTPUT gains, channel `c` at 188 + 6c | edit on 5 and 6 (212–223) |
| 248–255 | `00` | |

- **`02 00` is live, not a stored copy** (**device**, `research/rq1/live/`): all sixteen blocks read, one edit
  (input 3 gain −6 dB), all read again, the edit undone, all read once more. Only `02 00` and `02 01` followed the
  edit (bytes 140–145) and came back with the undo; `02 02` … `02 0E` did not move, so they are stored blocks
  (the USER SET and, **inferred**, the memories). In `02 0F` byte 5 changes between reads with nothing sent
  (`06`, `07`, `07`, …): a live value of unknown meaning; that block's layout is not the others' (`0F` runs).
- **It follows the front panel** (**device**, 6-Nov-2026, `research/rq1/tests/`, read with `ua1000ctl read`,
  nothing else sending): pressing STEREO/MONO on the device turned `mono` on in the block, and nothing else;
  pressing it again gave the block back as before.
- **It does NOT survive a power cycle: at power-on the device LOADS a stored state** (**device**,
  `research/rq1/rate/`). With the SAMPLE RATE switch on USER SET it loads the whole USER SET block `02 02` (panel,
  patch bay and mixer): outputs 9 and 10 had been set to 0 dB, the device was switched off and on, and `02 00` came
  back byte for byte equal to `02 02`, at −0.4 dB again. (A first power cycle that morning looked like "it keeps
  its state" only because the state then equalled `02 02`.) On a fixed rate (44.1 and 88.2 kHz measured) it loads
  another state, the same for both rates, which none of the sixteen blocks holds (the closest differs in 13 bytes):
  that morning it had inputs 1-8 panned hard left and right, inputs 3-4 at about −8 dB, input 9-10 unlinked, ALL on
  and SOFT CTRL off. Where that state is kept, and what saves it, is **unknown**.
- **Byte 4 of `02 00` says the SAMPLE RATE switch's position** (**device**): `00` at 44.1, `02` at 88.2, `09` on
  USER SET (stored at 48 kHz); `02 01` said `01` on both fixed rates. So bits 0–1 are the rate (0 = 44.1, 1 = 48,
  2 = 88.2, 3 = 96: 48 and 96 on a fixed position are **inferred**) and bit 3 = the USER SET position. The running
  rate is therefore not decoded from the block: the editor takes it from the driver, as it always did.
- **Writing the USER SET stores the whole current state** (**inferred**, strongly): the editor writes only
  `01 02 02 02`, the rate, the clock and switches bytes and zeros, as the vendor's panel does, yet the USER SET block
  read on 6-Nov held a full mixer (outputs 9-10 at −0.4 dB, set by a memory load on the night of 5-6 Nov before the
  last Store USER SET). The `02` bytes may be section flags, as in the memory messages.
- **Blocks `02 01` … `02 0F`** answer with the same layout. `02 01` and `02 02` were identical to `02 00` on the unit
  measured; `02 03` … `02 08` differ from it in switches, links, pans and gains. That `02 03` … `02 07` are the five
  memories (stored and loaded at `00 00 (slot + 2) …`, so slot 1 = `03`) is **inferred** from the numbering;
  for slot 5 it held (**device**, `research/rq1/tests/*_load5.txt`): `02 07` differed from `02 00` only in the
  pans of outputs 1 and 2, and loading memory 5 changed exactly those (to centre).

`ua1000ctl read` asks for block `02 00` and prints what it holds (`ua1000_state_from_block`, tested on the dumps of `research/rq1`), and the panel reads it when it connects (EditRolUA1000#18). A rawmidi input is started by the kernel only at its first read or poll, and what arrives before is lost: `ua1000_midi_open` polls once at once (a request sent before the panel first polled was never answered).
