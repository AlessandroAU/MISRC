/*
 * MISRC GUI - Soundcard FLAC Writer
 *
 * Writes captured soundcard audio to FLAC file.
 *
 * Copyright (C) 2024-2025 MISRC Authors
 * License: GPL-3.0-or-later
 */

#ifndef GUI_SOUNDCARD_WRITER_H
#define GUI_SOUNDCARD_WRITER_H

#include "../core/gui_app.h"
#include "../../common/buffer_manager.h"

/*
 * Start the soundcard FLAC writer thread.
 *
 * Reads from BUF_SOUNDCARD_AUDIO and writes to FLAC file.
 * Should be called when recording starts if soundcard capture is active.
 *
 * @param app   Application state
 * @param bufmgr Buffer manager instance
 * @return 0 on success, -1 on error
 */
int gui_soundcard_writer_start(gui_app_t *app, buffer_manager_t *bufmgr);

/*
 * Stop the soundcard FLAC writer thread.
 *
 * Drains remaining data and finishes the FLAC file.
 */
void gui_soundcard_writer_stop(void);

/*
 * Check if the soundcard writer is running.
 *
 * @return true if writer thread is active
 */
bool gui_soundcard_writer_is_running(void);

#endif // GUI_SOUNDCARD_WRITER_H
