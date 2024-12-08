/*
 * Copyright © 2024 Troeglazov Gerasim
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */

#include "cubeb-internal.h"
#include "cubeb/cubeb.h"
#include "cubeb_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Workaround for gcc_hidden.h hack of libxul.so
#pragma GCC visibility push(default)
#include <Application.h>
#include <Roster.h>
#include <SoundPlayer.h>
#include <String.h>
#include <NodeInfo.h>
#include <Path.h>
#include <OS.h>
#pragma GCC visibility pop

static const int MAX_CHANNELS = 2;
static const char DEFAULT_CONTEXT_NAME[] = "Cubeb";

extern "C" {
int haiku_init(cubeb ** context, char const * context_name);
}
static void haiku_destroy(cubeb* context);
static char const* haiku_get_backend_id(cubeb* context);
static int haiku_get_max_channel_count(cubeb* ctx, uint32_t* max_channels);
static int haiku_get_min_latency(cubeb* ctx, cubeb_stream_params params, uint32_t* latency_frames);
static int haiku_get_preferred_sample_rate(cubeb* ctx, uint32_t* rate);
static int haiku_stream_init(cubeb* context,
                 cubeb_stream** stream,
                 char const* stream_name,
                 cubeb_devid input_device,
                 cubeb_stream_params* input_stream_params,
                 cubeb_devid output_device,
                 cubeb_stream_params* output_stream_params,
                 unsigned int latency_frames,
                 cubeb_data_callback data_callback,
                 cubeb_state_callback state_callback,
                 void* user_ptr);
static void haiku_stream_destroy(cubeb_stream* stream);
static int haiku_stream_start(cubeb_stream* stream);
static int haiku_stream_stop(cubeb_stream* stream);
static int haiku_stream_get_position(cubeb_stream* stream, uint64_t* position);
static int haiku_stream_get_latency(cubeb_stream* stream, uint32_t* latency_frames);
static int haiku_stream_set_volume(cubeb_stream* stream, float volume);
static int haiku_stream_get_current_device(cubeb_stream* stream, cubeb_device** const device);
static int haiku_stream_device_destroy(cubeb_stream* stream, cubeb_device* device);
static int haiku_enumerate_devices(cubeb* context, cubeb_device_type type, cubeb_device_collection* collection);
static int haiku_device_collection_destroy(cubeb* context, cubeb_device_collection* collection);

static struct cubeb_ops const cubeb_haiku_ops = {
    .init = haiku_init,
    .get_backend_id = haiku_get_backend_id,
    .get_max_channel_count = haiku_get_max_channel_count,
    .get_min_latency = haiku_get_min_latency,
    .get_preferred_sample_rate = haiku_get_preferred_sample_rate,
    .get_supported_input_processing_params = NULL,
    .enumerate_devices = haiku_enumerate_devices,
    .device_collection_destroy = haiku_device_collection_destroy,
    .destroy = haiku_destroy,
    .stream_init = haiku_stream_init,
    .stream_destroy = haiku_stream_destroy,
    .stream_start = haiku_stream_start,
    .stream_stop = haiku_stream_stop,
    .stream_get_position = haiku_stream_get_position,
    .stream_get_latency = haiku_stream_get_latency,
    .stream_get_input_latency = NULL,
    .stream_set_volume = haiku_stream_set_volume,
    .stream_set_name = NULL,
    .stream_get_current_device = haiku_stream_get_current_device,
    .stream_set_input_mute = NULL,
    .stream_set_input_processing_params = NULL,
    .stream_device_destroy = haiku_stream_device_destroy,
    .stream_register_device_changed_callback = NULL,
    .register_device_collection_changed = NULL,
};

struct cubeb_stream {
  /* Note: Must match cubeb_stream layout in cubeb.c. */
  cubeb* context;
  void* user_ptr;
  /**/

  pthread_mutex_t mutex;

  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  cubeb_stream_params params;

  uint64_t position;
  bool pause;
  
  float volume;  

  BSoundPlayer* sound_player;
  media_raw_audio_format format;

  char stream_name[256];
};

struct cubeb {
  struct cubeb_ops const* ops;
  pthread_mutex_t mutex;

  bool active;
  uint32_t sample_rate;
  uint32_t latency;
  
  char context_name[256];
};

static void
haiku_audio_callback(void* cookie, void* buffer, size_t size, const media_raw_audio_format& format)
{
  cubeb_stream* stm = static_cast<cubeb_stream*>(cookie);
  
  if (buffer == nullptr || cookie == nullptr)
  	return;

  if (stm->pause) {
    memset(buffer, 0, size);
    return;
  }

  if (pthread_mutex_trylock(&stm->mutex) != 0) {
    memset(buffer, 0, size);
    return;
  }

  long frames = size / format.channel_count;
  switch (format.format) {
    case media_raw_audio_format::B_AUDIO_FLOAT:
      frames /= sizeof(float);
      break;
    case media_raw_audio_format::B_AUDIO_SHORT:
      frames /= sizeof(int16_t);
      break;
  }

  long got = stm->data_callback(stm, stm->user_ptr, nullptr, buffer, frames);

  if (got < 0) {
    memset(buffer, 0, size);
    pthread_mutex_unlock(&stm->mutex);
    stm->state_callback(stm, stm->user_ptr, CUBEB_STATE_ERROR);
    return;
  }

  stm->position += got;
  pthread_mutex_unlock(&stm->mutex);
}

static media_raw_audio_format
cubeb_format_to_haiku(cubeb_stream_params *params)
{
  media_raw_audio_format format;
  
  format.frame_rate = params->rate;
  format.channel_count = params->channels;
  format.buffer_size = 2048;
  format.byte_order = B_MEDIA_HOST_ENDIAN;
  
  switch (params->format) {
    case CUBEB_SAMPLE_FLOAT32NE:
      format.format = media_raw_audio_format::B_AUDIO_FLOAT;
      break;
    case CUBEB_SAMPLE_S16NE:
      format.format = media_raw_audio_format::B_AUDIO_SHORT;
      break;
    default:
      format.format = media_raw_audio_format::B_AUDIO_FLOAT;
      break;
  }
  
  return format;
}

int
haiku_init(cubeb** context, char const* context_name)
{
  *context = NULL;

  cubeb* ctx = (cubeb*)calloc(1, sizeof(*ctx));
  if (!ctx) {
    return CUBEB_ERROR;
  }

  ctx->ops = &cubeb_haiku_ops;
  ctx->mutex = PTHREAD_MUTEX_INITIALIZER;
  
  ctx->active = true;
  ctx->sample_rate = 48000;
  ctx->latency = 128;
  if (context_name)
    snprintf(ctx->context_name, 255, "%s", context_name);
  else
    snprintf(ctx->context_name, 255, "%s", DEFAULT_CONTEXT_NAME);

  *context = ctx;
  return CUBEB_OK;
}

static void
haiku_destroy(cubeb* context)
{
  context->active = false;
  free(context);
}

static char const*
haiku_get_backend_id(cubeb* context)
{
  return "haiku";
}

static int
haiku_get_max_channel_count(cubeb* ctx, uint32_t* max_channels)
{
  *max_channels = MAX_CHANNELS;
  return CUBEB_OK;
}

static int
haiku_get_min_latency(cubeb* ctx, cubeb_stream_params params, uint32_t* latency_frames)
{
  *latency_frames = 128;
  return CUBEB_OK;
}

static int
haiku_get_preferred_sample_rate(cubeb* ctx, uint32_t* rate)
{
  *rate = 48000;
  return CUBEB_OK;
}

static int
haiku_stream_init(cubeb* context,
                 cubeb_stream** stream,
                 char const* stream_name,
                 cubeb_devid input_device,
                 cubeb_stream_params* input_stream_params,
                 cubeb_devid output_device,
                 cubeb_stream_params* output_stream_params,
                 unsigned int latency_frames,
                 cubeb_data_callback data_callback,
                 cubeb_state_callback state_callback,
                 void* user_ptr)
{
  if (!output_stream_params) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  *stream = NULL;

  pthread_mutex_lock(&context->mutex);
  
  cubeb_stream* stm = reinterpret_cast<cubeb_stream *>(calloc(1, sizeof(*stm)));

  if (!stm) {
  	pthread_mutex_unlock(&context->mutex);    
    return CUBEB_ERROR;
  }

  stm->mutex = PTHREAD_MUTEX_INITIALIZER;
  if (stream_name) {
  	if (strcmp(stream_name, "AudioStream") == 0) {
  		snprintf(stm->stream_name, 255, "%s", context->context_name);
  	} else {
  		snprintf(stm->stream_name, 255, "%s", stream_name);
  	}
  } else {
  	snprintf(stm->stream_name, 255, "%s", context->context_name);
  }
  
  pthread_mutex_unlock(&context->mutex);

  pthread_mutex_lock(&stm->mutex);

  stm->context = context;
  stm->user_ptr = user_ptr;
  stm->params = *output_stream_params;
  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->position = 0;
  stm->volume = 1.0f;
  stm->format = cubeb_format_to_haiku(output_stream_params);
  stm->sound_player = nullptr;

  *stream = stm;
  pthread_mutex_unlock(&stm->mutex);

  return CUBEB_OK;
}

static void
haiku_stream_destroy(cubeb_stream* stream)
{
  if (!stream)
    return;

  haiku_stream_stop(stream);
}

static int
haiku_stream_start(cubeb_stream* stream)
{
  if (!stream->sound_player) {
    stream->sound_player = new BSoundPlayer(&stream->format,
                                     stream->stream_name,
                                     haiku_audio_callback,
                                     nullptr,
                                     stream);

    if (stream->sound_player->InitCheck() != B_OK)
      return CUBEB_ERROR;

    stream->pause = false;

    haiku_stream_set_volume(stream, stream->volume);

    stream->sound_player->Start();
    stream->sound_player->SetHasData(true);
  }

  stream->state_callback(stream, stream->user_ptr, CUBEB_STATE_STARTED);

  return CUBEB_OK;
}

static int
haiku_stream_stop(cubeb_stream* stream)
{
  if (stream->sound_player) {
    stream->pause = true;
    stream->sound_player->SetHasData(false);
    stream->sound_player->Stop();

    delete stream->sound_player;
    stream->sound_player = nullptr;
  }

  stream->state_callback(stream, stream->user_ptr, CUBEB_STATE_STOPPED);
  return CUBEB_OK;
}

static int
haiku_stream_get_position(cubeb_stream* stream, uint64_t* position)
{
  *position = stream->position;
  return CUBEB_OK;
}

static int
haiku_stream_get_latency(cubeb_stream* stream, uint32_t* latency_frames)
{
  if (!stream) {
    return CUBEB_ERROR;
  }

  *latency_frames = stream->format.buffer_size / 
                    (stream->format.channel_count * 
                     (stream->format.format == media_raw_audio_format::B_AUDIO_FLOAT ? 
                      sizeof(float) : sizeof(int16_t)));
  return CUBEB_OK;
}

static int
haiku_stream_set_volume(cubeb_stream* stream, float volume)
{
  if (!stream) {
    return CUBEB_ERROR;
  }
  
  stream->volume = volume;

  if (!stream->sound_player)
    return CUBEB_ERROR;
  
  stream->sound_player->SetVolume(volume);

  return CUBEB_OK;
}

static int
haiku_stream_get_current_device(cubeb_stream* stream, cubeb_device** const device)
{
  *device = (cubeb_device*)calloc(1, sizeof(cubeb_device));
  if (!*device) {
    return CUBEB_ERROR;
  }

  (*device)->output_name = strdup("Haiku Audio Output");
  (*device)->input_name = strdup("");
  
  return CUBEB_OK;
}

static int
haiku_stream_device_destroy(cubeb_stream* stream, cubeb_device* device)
{
  if (device->input_name) {
    free(device->input_name);
  }
  if (device->output_name) {
    free(device->output_name);
  }
  free(device);
  return CUBEB_OK;
}

static int
haiku_enumerate_devices(cubeb* context, cubeb_device_type type,
                       cubeb_device_collection* collection)
{
  if (!context || type != CUBEB_DEVICE_TYPE_OUTPUT) {
    return CUBEB_ERROR;
  }

  uint32_t rate;
  haiku_get_preferred_sample_rate(context, &rate);

  collection->count = 1;
  collection->device = new cubeb_device_info[1];
  
  cubeb_device_info* dev = &collection->device[0];
  memset(dev, 0, sizeof(cubeb_device_info));
  
  char const * a_name = "default";

  dev->device_id = a_name;
  dev->devid = (cubeb_devid)dev->device_id;
  dev->friendly_name = a_name;
  dev->group_id = a_name;
  dev->vendor_name = a_name;
  dev->type = CUBEB_DEVICE_TYPE_OUTPUT;
  dev->state = CUBEB_DEVICE_STATE_ENABLED;
  dev->preferred = CUBEB_DEVICE_PREF_ALL;
  dev->format = CUBEB_DEVICE_FMT_F32NE;
  dev->default_format = CUBEB_DEVICE_FMT_F32NE;
  dev->max_channels = MAX_CHANNELS;
  dev->min_rate = rate;
  dev->max_rate = rate;
  dev->default_rate = rate;
  dev->latency_lo = 0;
  dev->latency_hi = 0;

  return CUBEB_OK;
}

static int
haiku_device_collection_destroy(cubeb* context,
                              cubeb_device_collection* collection)
{
  delete[] collection->device;
  collection->device = nullptr;
  collection->count = 0;
  return CUBEB_OK;
}

