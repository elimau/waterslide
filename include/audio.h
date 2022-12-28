#ifndef _AUDIO_H
#define _AUDIO_H
#ifdef __cplusplus
extern "C" {
#endif

#include "ck/ck_ring.h"

// call these after stats_init()
int audio_init (ck_ring_t *ring, ck_ring_buffer_t *ringBuf, int fullRingSize);
int audio_start (const char *audioDeviceName);

#ifdef __cplusplus
}
#endif
#endif
