# The kernel patch: the UA-1000's control port

Linux plays and records through the UA-1000 out of the box, with the kernel's `snd-ua101` driver. What it cannot
do is reach the device's **mixers, patch bay, front-panel switches and memories**: the driver does not expose the
port they are controlled through. This document explains why, what the patch changes, how to build and load it,
and what has been tested.

- [The problem](#the-problem)
- [The patch](#the-patch)
- [Do I need it?](#do-i-need-it)
- [Building the module](#building-the-module)
- [Loading it](#loading-it)
- [Loading it at every boot](#loading-it-at-every-boot)
- [Going back to the stock driver](#going-back-to-the-stock-driver)
- [When the kernel is updated](#when-the-kernel-is-updated)
- [What has been tested](#what-has-been-tested)
- [Its status](#its-status)

## The problem

The UA-1000 (USB ID `0582:0044`) has one USB-MIDI interface with **two cables**:

| Cable | What it carries |
|---|---|
| 0 | The MIDI IN and MIDI OUT jacks on the rear panel |
| 1 | The device's control messages: Roland exclusive messages for its two mixers, patch bay, front-panel switches and memories, which the vendor's Windows control panel sends |

The USB captures of the vendor's panel show cable 1 on every control message, in both directions
([`PROTOCOL.md`](PROTOCOL.md), *captured*).

The kernel's driver declares **only cable 0**. The UA-1000 used to be driven by `snd-usb-audio`, whose quirk for it
declared both cables (`0x0003`). When it moved to `snd-ua101` (commit `e584bc3cf686`, "ALSA: ua101: add Edirol
UA-1000 support"), it took the UA-101's MIDI description, which has cable 0 alone. Since then:

- `amidi -l` lists one port for the device, `UA-1000 MIDI` (the jacks), and nothing for its control;
- whatever is sent to the device goes to the MIDI jacks, never to its mixer;
- the messages the device sends on cable 1 (a front-panel switch pressed, a memory dump) are dropped.

Audio is not affected: it travels on other USB interfaces.

## The patch

[`kernel/0001-ALSA-ua101-restore-the-control-port-of-the-UA-1000.patch`](../kernel/0001-ALSA-ua101-restore-the-control-port-of-the-UA-1000.patch)
declares both cables again, for the UA-1000 only:

| | Before | After |
|---|---|---|
| UA-1000 MIDI cables | `0x0001` (jacks) | `0x0003` (jacks and control) |
| `hw:N,0,0` | `UA-1000 MIDI`, the jacks | `UA-1000 MIDI`, the jacks: **unchanged** |
| `hw:N,0,1` | none | `UA-1000 Control`, the control port |
| UA-101 | cable 0 | cable 0: **unchanged** |

The jacks keep their port, so nothing that used them changes. The tools in this project find the control port by
its name, `UA-1000 Control`, not by its number.

It is one file, `sound/usb/misc/ua101.c`, 19 lines added and 7 removed. It carries `Fixes: e584bc3cf686` and
`Cc: stable@vger.kernel.org`, and its diff passes `checkpatch.pl --strict --no-signoff` with no error, warning or
check.

## Do I need it?

Run `amidi -l` with the UA-1000 connected and switched on:

```
Dir Device    Name
IO  hw:2,0,0  UA-1000 MIDI
IO  hw:2,0,1  UA-1000 Control      <- with this line, you do not need the patch
```

Without the `UA-1000 Control` line, the panel says NOT CONNECTED and `ua1000ctl` reports that the driver "exposes
no control port: it needs the cable 1 fix". The card number (`2` here) may differ.

## Building the module

`kernel/build-module.sh` builds the patched `snd-ua101.ko` for the **running** kernel, as a normal user,
without touching the system:

```sh
kernel/build-module.sh [KERNEL_TREE]
```

It needs a kernel tree of **exactly** the running kernel (`uname -r`), **with its driver sources**: the patch is
applied to a copy of the tree's `sound/usb/misc/ua101.c` (the tree itself is not written), and the driver includes two of the kernel's internal headers
(`sound/usb/usbaudio.h`, `midi.h`). A distribution's headers package usually lacks them; then pass a source tree of
the same version, configured from the running kernel and prepared:

```sh
cd <source tree of the running kernel's version>
cp /boot/config-$(uname -r) .config
make EXTRAVERSION=<the part of uname -r after the version> olddefconfig modules_prepare
<this repo>/kernel/build-module.sh "$PWD"
```

The script refuses a tree whose release string differs from `uname -r`: such a module would be rejected at load
time, or worse, loaded against another ABI. It prints the module's `vermagic` and where it wrote it
(`kernel/build-<release>/snd-ua101.ko`).

## Loading it

Loading needs root. With the UA-1000 connected:

```sh
sudo rmmod snd_ua101
# if it says "in use": the sound server holds the card. Stop it, then retry:
#   systemctl --user stop pipewire.socket pipewire wireplumber
sudo insmod kernel/build-$(uname -r)/snd-ua101.ko
systemctl --user start pipewire.socket pipewire wireplumber   # if you stopped it
amidi -l                                                      # UA-1000 Control must be listed
```

This lasts until the next reboot, or until the module is unloaded. Nothing on disk changes.

## Loading it at every boot

To have the patched module load by itself whenever the UA-1000 appears, install it where the system looks before
its own modules:

```sh
sudo install -D -m 644 kernel/build-$(uname -r)/snd-ua101.ko \
    /lib/modules/$(uname -r)/updates/snd-ua101.ko
sudo depmod -a
modinfo -n snd_ua101        # must print .../updates/snd-ua101.ko
```

The last line checks that the system will now pick the patched module. If it prints the stock module's path, your
distribution's `depmod` search order does not put `updates/` first: see `man depmod.d`.

## Going back to the stock driver

At any time:

```sh
sudo rmmod snd_ua101 && sudo modprobe snd_ua101     # until the next boot, or:
sudo rm /lib/modules/$(uname -r)/updates/snd-ua101.ko && sudo depmod -a   # if installed for every boot
```

## When the kernel is updated

A module is built for one kernel. After a kernel update the patched module no longer loads (the stock one is used,
and `amidi -l` loses the `UA-1000 Control` line): **build it again** for the new kernel, and install it again. A
module installed under `updates/` is per kernel version, so the new kernel simply does not see the old one.

Once the change is in the kernel itself, none of this is needed: see [Its status](#its-status).

## What has been tested

On 5-Nov-2026, on a desktop running Linux 7.0.10 (Liquorix), with the module built by `build-module.sh` and
loaded with `insmod`:

| Check | Result |
|---|---|
| The module loads | ✅ clean |
| `amidi -l` | ✅ `hw:2,0,0 UA-1000 MIDI` and `hw:2,0,1 UA-1000 Control` |
| `ua1000ctl ports` finds the control port | ✅ |
| Front-panel switches reach the computer (MONO, SOFT CTRL pressed on the device) | ✅ the panel on screen follows them |
| The panel and `ua1000ctl` change the device's settings | ✅ (the device check goes on: see the README) |
| Audio, with the patched module | ✅ playing and recording through PipeWire |
| The MIDI jacks' port (`hw:2,0,0`) opens for input and output | ✅ |
| MIDI data actually crossing the DIN jacks | ❌ **not tested, and will not be**: no MIDI cable or keyboard is available to this project. A report from anyone who can loop MIDI OUT to IN is welcome. |

The patch also builds without a warning (W=1) for Linux 6.12.113. It has not been tried on other kernels.

## Its status

The patch here is a **pilot**, deliberately unsigned (no `Signed-off-by`): it is the starting point for whoever
takes the change to the kernel, who writes and signs the patch that is sent. Until it is in the kernel, the
patched module has to be built for each kernel as above.

The device's driver is `sound/usb/misc/ua101.c`, in the kernel's sound (ALSA) subsystem; where changes to it are
sent is in the kernel's `MAINTAINERS` file (`scripts/get_maintainer.pl` on the patch prints it).
