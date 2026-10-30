#ifndef AMPPAGE_H
#define AMPPAGE_H

#include "src/gui/shell/gui.h"

extern lv_obj_t *amp_screen;
void amppage_init(gui_config_t *cfg);

// The catalog album's tracks, in the list page the search uses. False when
// a job is already running or the guest is not configured. The track menu
// calls it on a playing Apple Music track's album.
bool amppage_open_album(const char *album_id, const char *title);

#endif
