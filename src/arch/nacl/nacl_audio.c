/*
 *  Showtime Mediacenter
 *  Copyright (C) 2007-2014 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */

#include <unistd.h>

#include "ppapi/c/pp_errors.h"
#include "ppapi/c/pp_module.h"
#include "ppapi/c/pp_var.h"
#include "ppapi/c/ppb.h"
#include "ppapi/c/ppb_audio.h"
#include "ppapi/c/ppb_audio_config.h"
#include "ppapi/c/ppb_core.h"

#include "showtime.h"
#include "audio2/audio.h"
#include "media/media.h"

extern const PPB_Core *ppb_core;
extern const PPB_AudioConfig *ppb_audioconfig;
extern const PPB_Audio *ppb_audio;
extern PP_Instance g_Instance;

#define SLOTS 4
#define SLOTMASK (SLOTS - 1)

typedef struct decoder {
  audio_decoder_t ad;

  PP_Resource config;

  PP_Resource player;

  pthread_mutex_t mutex;
  pthread_cond_t cond;

  int16_t *samples;  // SLOTS * 2 * sizeof(uint16_t) * ad_tile_size

  int rdptr;
  int wrptr;


} decoder_t;



/**
 *
 */
static void
audio_cb(void *sample_buffer, uint32_t buffer_size_in_bytes,
         PP_TimeDelta latency, void *user_data)
{
  decoder_t *d = user_data;

  pthread_mutex_lock(&d->mutex);

  int off = (d->rdptr & SLOTMASK) * 2 * d->ad.ad_tile_size;
  memcpy(sample_buffer, d->samples + off, buffer_size_in_bytes);

  d->rdptr++;

  pthread_cond_signal(&d->cond);
  pthread_mutex_unlock(&d->mutex);
}


/**
 *
 */
static void
nacl_audio_fini(audio_decoder_t *ad)
{
  decoder_t *d = (decoder_t *)ad;

  free(d->samples);
  d->samples = NULL;

  if(d->player) {
    ppb_audio->StopPlayback(d->player);
    ppb_core->ReleaseResource(d->player);
    d->player = 0;
  }

  if(d->config) {
    ppb_core->ReleaseResource(d->config);
    d->config = 0;
  }

  pthread_mutex_destroy(&d->mutex);
  pthread_cond_destroy(&d->cond);
}


/**
 *
 */
static int
nacl_audio_reconfig(audio_decoder_t *ad)
{
  decoder_t *d = (decoder_t *)ad;

  nacl_audio_fini(ad);

  pthread_mutex_init(&d->mutex, NULL);
  pthread_cond_init(&d->cond, NULL);


  int sample_rate = ppb_audioconfig->RecommendSampleRate(g_Instance);

  int tile_size = ppb_audioconfig->RecommendSampleFrameCount(g_Instance,
                                                             sample_rate,
                                                             1024);

  d->config = ppb_audioconfig->CreateStereo16Bit(g_Instance,
                                                 sample_rate,
                                                 tile_size);

  ad->ad_out_sample_format = AV_SAMPLE_FMT_S16;
  ad->ad_out_sample_rate = sample_rate;
  ad->ad_out_channel_layout = AV_CH_LAYOUT_STEREO;
  ad->ad_tile_size = tile_size;

  d->samples = malloc(SLOTS * 2 * sizeof(uint16_t) * ad->ad_tile_size);

  d->player = ppb_audio->Create(g_Instance, d->config, audio_cb, ad);
  ppb_audio->StartPlayback(d->player);
  TRACE(TRACE_DEBUG, "AUDIO", "Audio playback started");
  return 0;
}


/**
 *
 */
static int
nacl_audio_deliver(audio_decoder_t *ad, int samples, int64_t pts, int epoch)
{
  decoder_t *d = (decoder_t *)ad;

  pthread_mutex_lock(&d->mutex);

  while((d->rdptr & SLOTMASK) == (d->wrptr & SLOTMASK) &&
        d->wrptr != d->rdptr)
    pthread_cond_wait(&d->cond, &d->mutex);

  pthread_mutex_unlock(&d->mutex);

  int off = (d->wrptr & SLOTMASK) * 2 * d->ad.ad_tile_size;

  uint8_t *data[8] = {0};
  data[0] = (uint8_t *)(d->samples + off);
  avresample_read(ad->ad_avr, data, samples);
  d->wrptr++;

  return 0;
}


/**
 *
 */
static void
nacl_audio_pause(audio_decoder_t *ad)
{
  decoder_t *d = (decoder_t *)ad;
  ppb_audio->StopPlayback(d->player);
}


/**
 *
 */
static void
nacl_audio_play(audio_decoder_t *ad)
{
  decoder_t *d = (decoder_t *)ad;
  ppb_audio->StartPlayback(d->player);
}


/**
 *
 */
static void
nacl_audio_flush(audio_decoder_t *ad)
{
}


/**
 *
 */
static audio_class_t nacl_audio_class = {
  .ac_alloc_size       = sizeof(decoder_t),
  .ac_fini             = nacl_audio_fini,
  .ac_reconfig         = nacl_audio_reconfig,
  .ac_deliver_unlocked = nacl_audio_deliver,
  .ac_pause            = nacl_audio_pause,
  .ac_play             = nacl_audio_play,
  .ac_flush            = nacl_audio_flush,
};



/**
 *
 */
audio_class_t *
audio_driver_init(struct prop *asettings, struct htsmsg *store)
{
  return &nacl_audio_class;
}

