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
   /* (patch 0018) the tiles of the batches submitted: memory traffic */
   RWP_PASSES,          /* batches with a fragment job */
   RWP_FULL_PASSES,     /* ... of them over 2 million pixels (the screen) */
   RWP_PRELOADS,        /* colour attachments read back into the tiles */
   RWP_ZS_PRELOADS,     /* depth/stencil attachments read back */
   RWP_PRELOAD_KB,      /* ... both, in KB over the drawn area */
   RWP_WRITEBACKS,      /* colour attachments written out */
   RWP_ZS_WRITEBACKS,   /* depth/stencil attachments written out */
   RWP_WRITEBACK_KB,
   RWP_CLEARS,          /* colour attachments cleared in the tiles */
   /* (patch 0019) where that traffic is: */
   RWP_REENTRY_PRELOADS,   /* colour preloads of a target written back earlier in the same work */
   RWP_REENTRY_KB,
   RWP_SHARED_KB,          /* preload + write-back KB of IOSurface (shared) targets */
   RWP_AFBC_KB,            /* ... of AFBC targets */
   RWP_LINEAR_KB,          /* ... of linear targets (the rest: 16x16 tiled) */
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

/*
 * (patch 0019) The passes of one command buffer's work, in the order submitted, for the profile:
 * redwood_pan_trace_on is set by air_metal.c for one span of work (rwa_pan_profile), and every
 * batch submitted meanwhile is recorded (at most RWP_TRACE). redwood_pan_span is air_metal.c's
 * count of works sent (a re-entry is a preload of a target written back in the same span).
 */
#define RWP_TRACE 40
struct redwood_pan_pass {
   uint16_t width, height;            /* the framebuffer */
   uint16_t extent_w, extent_h;       /* the drawn area */
   uint16_t res_w, res_h;             /* colour attachment 0's resource (or the depth one's) */
   uint16_t format;                   /* its pipe_format */
   uint8_t level, nr_cbufs;
   uint8_t kind;                      /* 0 linear, 1 16x16 tiled, 2 AFBC, 3 other */
   bool shared, preload, writeback, clear, reentry, zs, zs_preload, zs_writeback;
   uint32_t draws;
   const char *why;
   uint64_t first_seq, seq;           /* its GPU jobs' numbers, first and last (redwood-drm); 0: not known */
};
extern bool redwood_pan_trace_on;
extern unsigned redwood_pan_trace_n;
extern struct redwood_pan_pass redwood_pan_trace[RWP_TRACE];
extern uint64_t redwood_pan_span;

/* why batches with work were submitted: panfrost's own reason strings, counted */
#define RWP_REASONS 12
extern const char *redwood_pan_reason[RWP_REASONS];
extern uint64_t redwood_pan_reason_count[RWP_REASONS];
extern const char *redwood_pan_why;

/*
 * The Metal driver's passes (Mesa patch 0013, pan_job.c): whether a render batch drawing into
 * these attachments (the first colour attachment and the depth/stencil one, by value; either may
 * be NULL) is open, and submitting it. air_metal.c keeps a pass it leaves open for a few passes
 * more, so that one going back into the same attachments joins it.
 */
struct pipe_context;
struct pipe_surface;
int redwood_pan_pass_open(struct pipe_context *pctx, const struct pipe_surface *cbuf0,
                          const struct pipe_surface *zs);
int redwood_pan_pass_flush(struct pipe_context *pctx, const struct pipe_surface *cbuf0,
                           const struct pipe_surface *zs);
/*
 * Metal's load and store actions (Mesa patch 0018): redwood_pan_forget, MTLLoadActionDontCare --
 * the level's contents are not needed, so the next batch drawing it does not preload it (a 2D
 * resource of one layer only); redwood_pan_pass_discard, MTLStoreActionDontCare -- the open batch
 * of this pass does not write back the colour attachments in `color_mask` (bit i: attachment i)
 * nor the aspects in `zs_mask` (PIPE_CLEAR_DEPTH, PIPE_CLEAR_STENCIL); the number of batches.
 */
struct pipe_resource;
void redwood_pan_forget(struct pipe_resource *res, unsigned level);
int redwood_pan_pass_discard(struct pipe_context *pctx, const struct pipe_surface *cbuf0,
                             const struct pipe_surface *zs, unsigned color_mask, unsigned zs_mask);

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
