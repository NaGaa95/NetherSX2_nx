/* aaudio.c -- NDK AAudio shim backed by libnx audren
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "aaudio.h"
#include "pthr.h"
#include "util.h"

#define DEFAULT_FRAMES_PER_CB 512

#define NUM_WAVEBUFS 3

// ---------------------------------------------------------------------------
// audren config: stereo, 48 kHz, the default revision/mix layout. One mix
// (the final output mix) with stereo channels feeding the default device sink.
// ---------------------------------------------------------------------------
static const AudioRendererConfig k_audren_cfg = {
  .output_rate     = AudioRendererOutputRate_48kHz,
  .num_voices      = 4,
  .num_effects     = 0,
  .num_sinks       = 1,
  .num_mix_objs    = 1,
  .num_mix_buffers = 2,  // stereo
};

// ---------------------------------------------------------------------------
// Concrete definitions of the opaque types declared in aaudio.h. The core only
// ever holds these by pointer, so their layout is private to this file.
// ---------------------------------------------------------------------------
struct AAudioStreamBuilderStruct {
  int32_t sample_rate, channel_count, device_id;
  aaudio_format_t           format;
  aaudio_direction_t        direction;
  aaudio_sharing_mode_t     sharing;
  aaudio_performance_mode_t perf;
  int32_t buffer_capacity_frames, frames_per_cb;
  AAudioStream_dataCallback  data_cb;  void *data_user;
  AAudioStream_errorCallback error_cb; void *err_user;
};

struct AAudioStreamStruct {
  // resolved config (echo of builder, with negotiated values)
  int32_t sample_rate, channel_count, frames_per_cb;
  aaudio_format_t format;            // negotiated: FLOAT, or I16 on fallback
  AAudioStream_dataCallback  data_cb;  void *data_user;
  AAudioStream_errorCallback error_cb; void *err_user;

  // libnx audren side
  int               audren_inited;
  AudioDriver       drv;
  int               mempool_id;
  int               voice_id;
  AudioDriverWaveBuf wavebufs[NUM_WAVEBUFS];
  void             *ring;            // mempool-backed, page aligned
  size_t            ring_bytes;
  size_t            block_bytes;     // one cb block, in the device's sample fmt
  int               next_buf;        // round-robin index into wavebufs

  // playback thread + state
  pthread_t                  thread;
  int                        thread_started;
  volatile int               running;
  volatile int               thread_exited;   // set by playback_thread before it returns
  volatile int               thread_detached;
  volatile int               closing;
  volatile aaudio_stream_state_t state;
  volatile int64_t           frames_read;   // monotonic; AAudioStream_getFramesRead
  Mutex                      lock;           // guards state + cv
  Mutex                      control_lock;
  Mutex                      driver_lock;
  CondVar                    state_cv;       // signalled on every state change
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// libnx Result -> AAudio result. The core only branches on ==AAUDIO_OK vs not
// and logs the number, so a single generic error code is sufficient.
static aaudio_result_t map_result(Result rc) {
  return R_SUCCEEDED(rc) ? AAUDIO_OK : AAUDIO_ERROR_UNAVAILABLE;
}

// set state under lock and wake any waitForStateChange. Every transition must
// go through here so the condvar stays in sync with `state`.
static void set_state(AAudioStream *s, aaudio_stream_state_t st) {
  mutexLock(&s->lock);
  s->state = st;
  condvarWakeAll(&s->state_cv);
  mutexUnlock(&s->lock);
}

// bytes per interleaved stereo frame in the negotiated device format.
static size_t frame_bytes(AAudioStream *s) {
  size_t sample = (s->format == AAUDIO_FORMAT_PCM_I16) ? sizeof(int16_t)
                                                       : sizeof(float);
  return sample * (size_t)s->channel_count;
}

// ---------------------------------------------------------------------------
// builder
// ---------------------------------------------------------------------------

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder) {
  if (!builder) return AAUDIO_ERROR_NULL;
  AAudioStreamBuilder *b = calloc(1, sizeof(*b));
  if (!b) return AAUDIO_ERROR_NO_MEMORY;
  // sane defaults matching the AAudioMod request; the core overrides via setters
  b->sample_rate   = 48000;
  b->channel_count = 2;
  b->format        = AAUDIO_FORMAT_PCM_FLOAT;
  b->direction     = AAUDIO_DIRECTION_OUTPUT;
  b->sharing       = AAUDIO_SHARING_MODE_SHARED;
  b->perf          = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
  b->device_id     = 0;
  b->buffer_capacity_frames = 0;
  b->frames_per_cb = 0;  // 0 => we choose DEFAULT_FRAMES_PER_CB at openStream
  *builder = b;

  return AAUDIO_OK;
}

void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->device_id = v;
}
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t v) {
  if (b) b->direction = v;
}
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t v) {
  if (b) b->sharing = v;
}
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->sample_rate = v;
}
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->channel_count = v;
}
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t v) {
  if (b) b->format = v;
}
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->buffer_capacity_frames = v;
}
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t v) {
  if (b) b->perf = v;
}
void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder *b, int32_t v) {
  if (b) b->frames_per_cb = v;
}
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b,
                                         AAudioStream_dataCallback cb, void *user) {
  if (b) { b->data_cb = cb; b->data_user = user; }
}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b,
                                          AAudioStream_errorCallback cb, void *user) {
  if (b) { b->error_cb = cb; b->err_user = user; }
}

// Bring up audren and one stereo voice. We try FLOAT first (zero-copy from the
// SPU2 mixer); if audren rejects the float voice we drop to PCM_I16 and convert
// in the playback thread.
static aaudio_result_t open_audren(AAudioStream *s) {
  Result rc;

  rc = audrenInitialize(&k_audren_cfg);
  if (R_FAILED(rc)) {

    return map_result(rc);
  }
  s->audren_inited = 1;

  rc = audrvCreate(&s->drv, &k_audren_cfg, s->channel_count);
  if (R_FAILED(rc)) {

    audrenExit();
    s->audren_inited = 0;
    return map_result(rc);
  }

  // Ring buffer holding NUM_WAVEBUFS data-callback blocks back to back. audren
  // mempools must be page-aligned in both base and size; alignTo handles size.
  s->block_bytes = (size_t)s->frames_per_cb * frame_bytes(s);
  s->ring_bytes  = (s->block_bytes * NUM_WAVEBUFS + 0xFFF) & ~(size_t)0xFFF;
  s->ring = aligned_alloc(0x1000, s->ring_bytes);
  if (!s->ring) {

    audrvClose(&s->drv);
    audrenExit();
    s->audren_inited = 0;
    return AAUDIO_ERROR_NO_MEMORY;
  }
  memset(s->ring, 0, s->ring_bytes);
  armDCacheFlush(s->ring, s->ring_bytes);

  s->mempool_id = audrvMemPoolAdd(&s->drv, s->ring, s->ring_bytes);
  if (s->mempool_id < 0) {

    goto fail_pool;
  }
  audrvMemPoolAttach(&s->drv, s->mempool_id);

  // Route the voice to the default output mix.
  static const u8 sink_channels[] = { 0, 1 };
  audrvDeviceSinkAdd(&s->drv, AUDREN_DEFAULT_DEVICE_NAME,
                     s->channel_count, sink_channels);
  audrvUpdate(&s->drv);

  s->voice_id = 0;
  PcmFormat pcm = (s->format == AAUDIO_FORMAT_PCM_I16) ? PcmFormat_Int16
                                                       : PcmFormat_Float;
  bool ok = audrvVoiceInit(&s->drv, s->voice_id, s->channel_count, pcm,
                           (float)s->sample_rate);
  if (!ok && s->format != AAUDIO_FORMAT_PCM_I16) {
    // Audren may reject float output.
    s->format = AAUDIO_FORMAT_PCM_I16;
    // resize ring for the now-smaller frame size
    free(s->ring);
    s->block_bytes = (size_t)s->frames_per_cb * frame_bytes(s);
    s->ring_bytes  = (s->block_bytes * NUM_WAVEBUFS + 0xFFF) & ~(size_t)0xFFF;
    s->ring = aligned_alloc(0x1000, s->ring_bytes);
    if (!s->ring) { goto fail_voice; }
    memset(s->ring, 0, s->ring_bytes);
    armDCacheFlush(s->ring, s->ring_bytes);
    audrvMemPoolDetach(&s->drv, s->mempool_id);
    audrvMemPoolRemove(&s->drv, s->mempool_id);
    s->mempool_id = audrvMemPoolAdd(&s->drv, s->ring, s->ring_bytes);
    if (s->mempool_id < 0) goto fail_voice;
    audrvMemPoolAttach(&s->drv, s->mempool_id);
    ok = audrvVoiceInit(&s->drv, s->voice_id, s->channel_count,
                        PcmFormat_Int16, (float)s->sample_rate);
  }
  if (!ok) {

    goto fail_voice;
  }

  audrvVoiceSetDestinationMix(&s->drv, s->voice_id, AUDREN_FINAL_MIX_ID);
  // straight stereo: L->mixbuf0, R->mixbuf1 at unity, no cross-feed.
  audrvVoiceSetMixFactor(&s->drv, s->voice_id, 1.0f, 0, 0);
  audrvVoiceSetMixFactor(&s->drv, s->voice_id, 1.0f, 1, 1);
  if (s->channel_count >= 2) {
    audrvVoiceSetMixFactor(&s->drv, s->voice_id, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&s->drv, s->voice_id, 0.0f, 0, 1);
  }
  audrvVoiceStart(&s->drv, s->voice_id);

  audrvUpdate(&s->drv);

  rc = audrenStartAudioRenderer();
  if (R_FAILED(rc)) {

    goto fail_voice;
  }


  return AAUDIO_OK;

fail_voice:
  if (s->mempool_id >= 0) {
    audrvMemPoolDetach(&s->drv, s->mempool_id);
    audrvMemPoolRemove(&s->drv, s->mempool_id);
  }
fail_pool:
  free(s->ring);
  s->ring = NULL;
  audrvClose(&s->drv);
  audrenExit();
  s->audren_inited = 0;
  return AAUDIO_ERROR_UNAVAILABLE;
}

aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b,
                                               AAudioStream **stream) {
  if (!b || !stream) return AAUDIO_ERROR_NULL;
  if (!b->data_cb) {
    // AAudioMod always sets a data callback; without it we have no source.

    return AAUDIO_ERROR_NULL;
  }

  AAudioStream *s = calloc(1, sizeof(*s));
  if (!s) return AAUDIO_ERROR_NO_MEMORY;

  s->sample_rate   = b->sample_rate   > 0 ? b->sample_rate   : 48000;
  s->channel_count = b->channel_count > 0 ? b->channel_count : 2;
  s->frames_per_cb = b->frames_per_cb > 0 ? b->frames_per_cb : DEFAULT_FRAMES_PER_CB;
  s->format        = (b->format == AAUDIO_FORMAT_PCM_I16) ? AAUDIO_FORMAT_PCM_I16
                                                          : AAUDIO_FORMAT_PCM_FLOAT;
  s->data_cb   = b->data_cb;   s->data_user = b->data_user;
  s->error_cb  = b->error_cb;  s->err_user  = b->err_user;
  s->mempool_id = -1;
  s->voice_id   = -1;
  mutexInit(&s->lock);
  mutexInit(&s->control_lock);
  mutexInit(&s->driver_lock);
  condvarInit(&s->state_cv);



  aaudio_result_t r = open_audren(s);
  if (r != AAUDIO_OK) {
    free(s);
    return r;
  }

  set_state(s, AAUDIO_STREAM_STATE_OPEN);
  *stream = s;
  return AAUDIO_OK;
}

aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) {
  free(b);
  return AAUDIO_OK;
}

// ---------------------------------------------------------------------------
// playback thread: pull from the core, feed audren, advance frames_read
// ---------------------------------------------------------------------------

// is this wavebuf free to refill? Done/Free means audren is no longer reading
// it; a never-queued slot has state 0 (== AudioDriverWaveBufState_Free).
static int wavebuf_is_free(AudioDriverWaveBuf *wb) {
  return wb->state == AudioDriverWaveBufState_Free ||
         wb->state == AudioDriverWaveBufState_Done;
}

static int stream_is_running(AAudioStream *s) {
  return __atomic_load_n(&s->running, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE);
}

static void *playback_thread(void *arg) {
  AAudioStream *s = (AAudioStream *)arg;

  pthr_pin_bg_core();
  pthr_set_priority(43);  // shortest wakeups of the core-3 tenants
  pthr_ensure_fake_tls();

  set_state(s, AAUDIO_STREAM_STATE_STARTED);


  while (stream_is_running(s)) {
    AudioDriverWaveBuf *wb = &s->wavebufs[s->next_buf];
    mutexLock(&s->driver_lock);
    const int free = wavebuf_is_free(wb);
    mutexUnlock(&s->driver_lock);
    if (!free) {
      // all slots in flight -- wait one audren mix tick and re-poll.
      audrenWaitFrame();
      if (!stream_is_running(s))
        break;
      mutexLock(&s->driver_lock);
      if (stream_is_running(s))
        audrvUpdate(&s->drv);
      mutexUnlock(&s->driver_lock);
      continue;
    }

    // The callback writes in the negotiated stream format.
    void *block = (uint8_t *)s->ring + (size_t)s->next_buf * s->block_bytes;
    void *cb_dst = block;

    aaudio_data_callback_result_t cr =
        s->data_cb((AAudioStream *)s, s->data_user, cb_dst, s->frames_per_cb);

    if (!stream_is_running(s))
      break;

    // hand the freshly filled block to audren.
    armDCacheFlush(block, s->block_bytes);
    mutexLock(&s->driver_lock);
    if (!stream_is_running(s)) {
      mutexUnlock(&s->driver_lock);
      break;
    }
    memset(wb, 0, sizeof(*wb));
    wb->data_raw      = s->ring;
    wb->size          = s->ring_bytes;
    wb->start_sample_offset = (s32)((size_t)s->next_buf * s->frames_per_cb);
    wb->end_sample_offset   = wb->start_sample_offset + s->frames_per_cb;
    audrvVoiceAddWaveBuf(&s->drv, s->voice_id, wb);
    audrvUpdate(&s->drv);
    mutexUnlock(&s->driver_lock);

    s->frames_read += s->frames_per_cb;   // monotonic; AAudioStream_getFramesRead
    s->next_buf = (s->next_buf + 1) % NUM_WAVEBUFS;

    if (cr == AAUDIO_CALLBACK_RESULT_STOP) {
      __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
      break;
    }

    // block until audren has consumed a frame's worth before refilling.
    audrenWaitFrame();
  }


  __atomic_store_n(&s->thread_exited, 1, __ATOMIC_RELEASE);
  return NULL;
}

// Do not block VM reset on a callback that is waiting for a core lock.
static int stop_playback_thread(AAudioStream *s) {
  if (!s->thread_started) return 1;
  __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
  for (int i = 0; i < 300; i++) {
    if (__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {
      pthread_join(s->thread, NULL);
      s->thread_started = 0;
      return 1;
    }
    svcSleepThread(5000000ULL);
  }

  pthread_detach(s->thread);
  s->thread_started = 0;
  s->thread_detached = 1;
  return 0;
}

// ---------------------------------------------------------------------------
// stream control
// ---------------------------------------------------------------------------

aaudio_result_t AAudioStream_requestStart(AAudioStream *s) {
  if (!s) return AAUDIO_ERROR_NULL;
  mutexLock(&s->control_lock);
  if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE)) {
    mutexUnlock(&s->control_lock);
    return AAUDIO_ERROR_DISCONNECTED;
  }

  set_state(s, AAUDIO_STREAM_STATE_STARTING);

  if (s->thread_started && __atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {
    pthread_join(s->thread, NULL);
    s->thread_started = 0;
  }
  if (s->thread_started) {
    set_state(s, AAUDIO_STREAM_STATE_STARTED);
    mutexUnlock(&s->control_lock);
    return AAUDIO_OK;
  }

  // If a previous stop had to detach a wedged thread, make sure it has actually
  // exited before spawning a new one -- else two threads would drive `s` at once.
  if (s->thread_detached && !__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {
    for (int i = 0; i < 400 && !__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE); i++)
      svcSleepThread(5000000ULL); // wait up to 2s for the old thread to drain
    if (!__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {

      set_state(s, AAUDIO_STREAM_STATE_OPEN);
      mutexUnlock(&s->control_lock);
      return AAUDIO_ERROR_UNAVAILABLE;
    }
  }
  s->thread_detached = 0;
  s->thread_exited = 0;   // fresh thread; must be cleared BEFORE pthread_create

  mutexLock(&s->driver_lock);
  if (!s->audren_inited) {
    mutexUnlock(&s->driver_lock);
    mutexUnlock(&s->control_lock);
    return AAUDIO_ERROR_DISCONNECTED;
  }
  if (s->voice_id >= 0) {
    audrvVoiceStart(&s->drv, s->voice_id);
    audrvUpdate(&s->drv);
  }
  mutexUnlock(&s->driver_lock);
  __atomic_store_n(&s->running, 1, __ATOMIC_RELEASE);
  // spawn via newlib pthreads directly (this is OUR thread, not core-spawned);
  // it pins itself to a bg core in playback_thread.
  int rc = pthread_create(&s->thread, NULL, playback_thread, s);
  if (rc != 0) {
    __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);

    set_state(s, AAUDIO_STREAM_STATE_OPEN);
    mutexUnlock(&s->control_lock);
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  s->thread_started = 1;
  // playback_thread sets STATE_STARTED once it's running.
  mutexUnlock(&s->control_lock);
  return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestPause(AAudioStream *s) {
  if (!s) return AAUDIO_ERROR_NULL;
  mutexLock(&s->control_lock);
  if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE)) {
    mutexUnlock(&s->control_lock);
    return AAUDIO_ERROR_DISCONNECTED;
  }

  set_state(s, AAUDIO_STREAM_STATE_PAUSING);
  stop_playback_thread(s);
  mutexLock(&s->driver_lock);
  if (s->voice_id >= 0) {
    audrvVoiceSetPaused(&s->drv, s->voice_id, true);
    audrvUpdate(&s->drv);
  }
  mutexUnlock(&s->driver_lock);
  set_state(s, AAUDIO_STREAM_STATE_PAUSED);
  mutexUnlock(&s->control_lock);
  return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestStop(AAudioStream *s) {
  if (!s) return AAUDIO_ERROR_NULL;
  mutexLock(&s->control_lock);
  if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE)) {
    mutexUnlock(&s->control_lock);
    return AAUDIO_ERROR_DISCONNECTED;
  }

  set_state(s, AAUDIO_STREAM_STATE_STOPPING);
  stop_playback_thread(s);
  mutexLock(&s->driver_lock);
  if (s->voice_id >= 0) {
    audrvVoiceStop(&s->drv, s->voice_id);
    audrvUpdate(&s->drv);
  }
  mutexUnlock(&s->driver_lock);
  set_state(s, AAUDIO_STREAM_STATE_STOPPED);
  mutexUnlock(&s->control_lock);
  return AAUDIO_OK;
}

aaudio_result_t AAudioStream_close(AAudioStream *s) {
  if (!s) return AAUDIO_ERROR_NULL;
  mutexLock(&s->control_lock);
  if (__atomic_exchange_n(&s->closing, 1, __ATOMIC_ACQ_REL)) {
    mutexUnlock(&s->control_lock);
    return AAUDIO_OK;
  }

  set_state(s, AAUDIO_STREAM_STATE_CLOSING);

  int joined = stop_playback_thread(s);

  mutexLock(&s->driver_lock);
  if (s->audren_inited) {
    if (s->voice_id >= 0)
      audrvVoiceStop(&s->drv, s->voice_id);
    if (s->mempool_id >= 0) {
      audrvMemPoolDetach(&s->drv, s->mempool_id);
      audrvMemPoolRemove(&s->drv, s->mempool_id);
    }
    audrvUpdate(&s->drv);
    audrvClose(&s->drv);
    audrenStopAudioRenderer();  // pairs with audrenStartAudioRenderer()
    audrenExit();
    s->audren_inited = 0;
  }
  mutexUnlock(&s->driver_lock);

  set_state(s, AAUDIO_STREAM_STATE_CLOSED);
  // If we had to detach a wedged playback thread and it still hasn't exited, it
  // may still touch `s`/`s->ring` -> leak them rather than free-then-UAF. This is
  // a rare teardown-race path; a one-shot leak at VM reset/exit is acceptable.
  if (!joined && !__atomic_load_n(&s->thread_exited, __ATOMIC_ACQUIRE)) {

    mutexUnlock(&s->control_lock);
    return AAUDIO_OK;
  }
  free(s->ring);
  s->ring = NULL;
  mutexUnlock(&s->control_lock);
  return AAUDIO_OK;
}

int64_t AAudioStream_getFramesRead(AAudioStream *s) {
  // monotonic counter the core uses for A/V sync and its disconnect/
  // "counter mismatch" reopen check; MUST advance while playing.
  return s ? s->frames_read : 0;
}

aaudio_result_t AAudioStream_waitForStateChange(AAudioStream *s,
                                                aaudio_stream_state_t inputState,
                                                aaudio_stream_state_t *nextState,
                                                int64_t timeoutNanoseconds) {
  if (!s) return AAUDIO_ERROR_NULL;

  mutexLock(&s->lock);
  if (s->state != inputState) {
    // already moved on -- report current state immediately.
    if (nextState) *nextState = s->state;
    mutexUnlock(&s->lock);
    return AAUDIO_OK;
  }

  // wait for the next set_state() signal, bounded by the caller's timeout.
  // condvarWaitTimeout returns 0xEA01 (timeout) when the deadline expires.
  Result rc = condvarWaitTimeout(&s->state_cv, &s->lock,
                                 (u64)(timeoutNanoseconds > 0 ? timeoutNanoseconds : 0));
  aaudio_stream_state_t cur = s->state;
  mutexUnlock(&s->lock);

  if (R_FAILED(rc) && cur == inputState) {
    // timed out without a transition; the core tolerates and retries.
    return AAUDIO_ERROR_TIMEOUT;
  }
  if (nextState) *nextState = cur;
  return AAUDIO_OK;
}
