#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build snd-ua101.ko with the cable-1 patch for the RUNNING kernel, out of
# tree, without root.  Loading it (rmmod snd_ua101; insmod snd-ua101.ko) is
# left to the user: it needs root.
#
# Usage: build-module.sh [KERNEL_TREE]
#   OUT=<dir>  where to build (default kernel/build-$(uname -r));
#              an existing OUT is emptied only if it is this script's output
#
# KERNEL_TREE defaults to /lib/modules/$(uname -r)/build (the distribution's
# headers package).  When the headers of the running kernel are no longer
# available, pass a source tree of the SAME kernel, configured with the
# running kernel's /boot/config-* and prepared with
#   make EXTRAVERSION=<suffix> olddefconfig modules_prepare
# so that `make -s kernelrelease` prints exactly `uname -r`.  The script
# refuses any tree whose release string differs: such a module would be
# rejected at load time, or worse, loaded against another ABI.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
patch="$here/0001-ALSA-ua101-restore-the-control-port-of-the-UA-1000.patch"
release=$(uname -r)
tree=${1:-/lib/modules/$release/build}
out=${OUT:-$here/build-$release}

if [ ! -f "$tree/Makefile" ]; then
	echo "no kernel tree at $tree (install the headers of $release, or pass a prepared source tree)" >&2
	exit 1
fi

# EXTRAVERSION is only needed (and only passed) for a source tree whose
# Makefile names another suffix; a headers package already has the right one.
extra=
tree_release=$(make -s -C "$tree" kernelrelease 2>/dev/null || true)
if [ "$tree_release" != "$release" ]; then
	base=$(make -s -C "$tree" EXTRAVERSION= kernelversion)
	extra="EXTRAVERSION=${release#"$base"}"
	tree_release=$(make -s -C "$tree" "$extra" kernelrelease)
fi
if [ "$tree_release" != "$release" ]; then
	echo "tree release '$tree_release' differs from the running kernel '$release'" >&2
	exit 1
fi

src="$tree/sound/usb/misc/ua101.c"
if [ ! -f "$src" ]; then
	echo "$src is missing: a headers package has no driver sources; pass a full source tree" >&2
	exit 1
fi

# $out is emptied first: only when it is this script's own output (its Kbuild
# names the module), never another directory OUT happens to name.
if [ -e "$out" ]; then
	if ! grep -qs '^obj-m := snd-ua101.o$' "$out/Kbuild"; then
		echo "$out exists and is not this script's output: refusing to empty it (set OUT elsewhere)" >&2
		exit 1
	fi
	rm -rf "$out"
fi
mkdir -p "$out"
cp "$src" "$out/ua101.c"
sed 's#sound/usb/misc/##g' "$patch" | (cd "$out" && patch -p1 -s)
cat > "$out/Kbuild" <<EOF
obj-m := snd-ua101.o
snd-ua101-y := ua101.o
ccflags-y := -I$tree/sound/usb/misc
EOF

# A prepared source tree has no Module.symvers (that needs a full build):
# modpost cannot see the exported symbols and would refuse.  Without
# CONFIG_MODVERSIONS the kernel resolves them by name at load time, so the
# check is turned into warnings only in that case.
warn=
if [ ! -f "$tree/Module.symvers" ]; then
	if grep -q '^CONFIG_MODVERSIONS=y' "$tree/.config"; then
		echo "CONFIG_MODVERSIONS is set and $tree has no Module.symvers: cannot build safely" >&2
		exit 1
	fi
	warn="KBUILD_MODPOST_WARN=1"
fi

make -C "$tree" ${extra:+"$extra"} M="$out" W=1 $warn modules

/sbin/modinfo -F vermagic "$out/snd-ua101.ko"
echo "built: $out/snd-ua101.ko"
