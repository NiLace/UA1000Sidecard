/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000_statefile.h - where the editor's state lives on disk
 *
 * One file, shared by ua1000ctl and ua1000gui:
 * $XDG_STATE_HOME/ua1000/state, or ~/.local/state/ua1000/state.
 */
#ifndef UA1000_STATEFILE_H
#define UA1000_STATEFILE_H

#include "ua1000_state.h"

/*
 * Write the default path to path.  Returns 0, or -ENOENT when neither
 * XDG_STATE_HOME nor HOME is set to an absolute path, or -ENAMETOOLONG when
 * the path does not fit in len bytes.
 */
int ua1000_statefile_default_path(char *path, size_t len);

/*
 * Load the state.  A missing file is not an error: *state gets the defaults
 * and *existed is set to false.  Returns 0, or a negative errno with a
 * description in err: -EISDIR for a directory, -EINVAL for a malformed
 * file (*state then holds what was read before the bad line), -EIO for a
 * read error, or the error fopen() met.
 */
int ua1000_statefile_load(const char *path, struct ua1000_state *state,
			  bool *existed, char *err, size_t err_len);

/*
 * Save the state atomically (a temporary file renamed over the old one,
 * both synced to disk), creating the missing directories with mode 0700.
 * Returns 0 or a negative errno.  A failure before the rename leaves the
 * old file untouched and no temporary file behind; one after it (syncing
 * the directory) leaves the new file in place.
 */
int ua1000_statefile_save(const char *path, const struct ua1000_state *state);

#endif /* UA1000_STATEFILE_H */
