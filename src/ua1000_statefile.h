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
 * XDG_STATE_HOME nor HOME is set, or -ENAMETOOLONG.
 */
int ua1000_statefile_default_path(char *path, size_t len);

/*
 * Load the state.  A missing file is not an error: *state gets the defaults
 * and *existed is set to false.  Returns 0, or a negative errno with a
 * description in err.
 */
int ua1000_statefile_load(const char *path, struct ua1000_state *state,
			  bool *existed, char *err, size_t err_len);

/*
 * Save the state atomically (a temporary file renamed over the old one),
 * creating the directory if needed.  Returns 0 or a negative errno.
 */
int ua1000_statefile_save(const char *path, const struct ua1000_state *state);

#endif /* UA1000_STATEFILE_H */
