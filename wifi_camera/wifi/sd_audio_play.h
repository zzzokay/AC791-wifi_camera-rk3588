#ifndef __SD_AUDIO_PLAY_H__
#define __SD_AUDIO_PLAY_H__

#include "system/includes.h"

int sd_audio_play_start(const char *path, const char *dec_type, u8 volume);
int sd_audio_play_stop(void);
int sd_audio_play_busy(void);

#endif
