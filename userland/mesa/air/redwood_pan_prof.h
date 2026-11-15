/*
 * Copyright 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 *
 * A34: where panfrost's processor time goes in a process that draws through Metal. The counters
 * are panfrost's (patch 0011, defined in pan_job.c); air_metal.c reads and clears them for the
 * plug-in's profile (rwa_pan_profile, the "rwm pan" lines of camprof). In a process that only
 * uses OpenGL ES (redwood_pan_metal false) nothing is timed or counted.
 *
 * Included by panfrost (pan_job.c, pan_cmdstream.c, pan_mempool.c, pan_bo.c) and by
 * air_metal.c, so it includes nothing of either.
 */
#ifndef REDWOOD_PAN_PROF_H
#define REDWOOD_PAN_PROF_H

#include <stdbool.h>
#include <stdint.h>

#include "util/os_time.h"

enum redwood_pan_prof_index {
   RWP_DRAWS,           /* draws through panfrost_single_draw_direct */
   RWP_PREPARE,         /* ns: prepare_draw -- the batch found or begun, the viewport */
   RWP_VCOUNT,          /* ns: the active primitive, the vertex count, the index buffer */
   RWP_ZS,              /* ns: update_state_3d, depth/stencil descriptor */
   RWP_BLEND,           /* ns: update_state_3d, blend descriptors (and blend shaders) */
   RWP_VERTEX,          /* ns: update_state_3d, attributes and vertex buffers */
   RWP_STATE3D,         /* ns: update_state_3d, all of it */
   RWP_TEX,             /* ns: update_shader_state, texture descriptors, both stages */
   RWP_SAMP,            /* ns: sampler descriptors */
   RWP_SHADER,          /* ns: shader program and varying descriptors */
   RWP_UBO,             /* ns: uniform buffers, sysvals and push constants */
   RWP_IMG,             /* ns: images and storage buffers */
   RWP_STAGES,          /* ns: update_shader_state, both stages, all of it */
   RWP_LAUNCH,          /* ns: the job written (launch_draw) */
   RWP_REBINDS,         /* panfrost_get_render_batch with no current batch: all state dirty */
   RWP_BATCH_INITS,     /* render and compute batches begun */
   RWP_BATCH_INIT_NS,
   RWP_POOL_ALLOCS,     /* a pool's new backing buffer (panfrost_pool_alloc_backing) */
   RWP_POOL_ALLOC_NS,
   RWP_BO_CREATES,      /* panfrost_bo_create */
   RWP_BO_CREATE_NS,
   RWP_BO_FRESH,        /* ... of them not from Mesa's cache (a CREATE_BO of the kernel) */
   RWP_BO_FRESH_NS,
   RWP_DEP_FLUSHES,     /* batches submitted from inside a draw: an access to what they use */
   RWP_DEP_FLUSH_NS,
   RWP_EMPTY_SUBMITS,   /* batches let go with nothing in them */
   RWP_JOB_SUBMITS,     /* batches with work, submitted */
   RWP_JOB_SUBMIT_NS,
   RWP_SLOTS
};

extern uint64_t redwood_pan_prof[RWP_SLOTS];
extern bool redwood_pan_metal;

/*
 * What the processor's stores and loads cost in a batch pool's memory -- where every descriptor
 * of every draw is written -- against ordinary memory, measured once, at the first batch begun
 * in a Metal process (pan_job.c): ns for 4 KB written a 32-bit word at a time (as pan_pack
 * does), copied with memcpy, and read a word at a time; the pool's first, then malloc's. Kept,
 * not cleared by the profile.
 */
enum redwood_pan_memtest_index {
   RWP_MEM_POOL_STORE, RWP_MEM_POOL_COPY, RWP_MEM_POOL_LOAD,
   RWP_MEM_RAM_STORE, RWP_MEM_RAM_COPY, RWP_MEM_RAM_LOAD,
   RWP_MEM_SLOTS
};
extern uint64_t redwood_pan_memtest[RWP_MEM_SLOTS];

/* why batches with work were submitted: panfrost's own reason strings, counted */
#define RWP_REASONS 12
extern const char *redwood_pan_reason[RWP_REASONS];
extern uint64_t redwood_pan_reason_count[RWP_REASONS];
extern const char *redwood_pan_why;

static inline uint64_t
rwp_now(void)
{
   return redwood_pan_metal ? os_time_get_nano() : 0;
}

/* the time since *t into slot i, and *t moved on to now */
static inline void
rwp_lap(unsigned i, uint64_t *t)
{
   if (redwood_pan_metal) {
      uint64_t now = os_time_get_nano();
      redwood_pan_prof[i] += now - *t;
      *t = now;
   }
}

static inline void
rwp_count(unsigned i)
{
   if (redwood_pan_metal)
      redwood_pan_prof[i]++;
}

#endif
