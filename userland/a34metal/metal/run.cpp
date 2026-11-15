/*
 * A command buffer's work, run when it is committed -- Metal's rule, kept: the recorded commands run here and not
 * when they were encoded, in the order they were encoded.
 */
#include "rwm.h"

#include <stdio.h>
#include <string.h>

namespace {

/* the first, second, fourth... time a site says it, with the count after the first (the plug-in's RW_AGAIN) */
bool
again(unsigned long &times)
{
	return (++times & (times - 1)) == 0;
}

void
say_again(unsigned long &times, const char *what, const char *why)
{
	if (again(times)) {
		rwm_print("%s: %s\n", what, why);
		if (times > 1) {
			rwm_print("  ... and that is the %lu'th time\n", times);
		}
	}
}

/* the targets of a command that are over a surface, once each */
void
note_drawn(const struct rwm_work *w, struct rwm_texture **drawn, unsigned &ndrawn, unsigned room)
{
	for (unsigned i = 0; i < w->target_count; i++) {
		struct rwm_texture *t = w->targets[i];
		unsigned k;

		if (t == nullptr || !rwm_texture_has_surface(t)) {
			continue;
		}
		for (k = 0; k < ndrawn && drawn[k] != t; k++) {
		}
		if (k == ndrawn && ndrawn < room) {
			drawn[ndrawn++] = t;
		}
	}
}

} // namespace

/* a texture over another process's surface brought up to date, counted in rwm_perf (also -[RedwoodMetalTexture
 * redwoodCopyInNow]) */
extern "C" void
rwm_copy_in(struct rwm_texture *texture)
{
	static unsigned long times;
	char why[256] = { 0 };
	size_t bytes = 0;
	const uint64_t began = rwm_ns();

	if (rwm_texture_copy_in(texture, &bytes, why, sizeof(why)) != 0) {
		say_again(times, "copy-in: the surface could not be written into its texture", why);
	}
	if (bytes != 0) {
		rwm_perf.copyins++;
		rwm_perf.copyin_bytes += bytes;
		rwm_perf.copyin_ns += rwm_ns() - began;
	}
}

/* at -commit with the driver's worker: the surfaces the works sample copied in as they are now */
extern "C" void
rwm_works_copy_in(struct rwm_work *const *works, unsigned count)
{
	for (unsigned n = 0; n < count; n++) {
		for (unsigned i = 0; i < works[n]->copy_in_count; i++) {
			rwm_copy_in(works[n]->copy_in[i]);
		}
	}
}

/* A34: one draw into the traced command buffer's passes, by pipeline (rwm_pass_draws) */
static void
note_pass_draw(const struct rwm_work *w)
{
	static const struct rwa_texture *last_target;
	static unsigned last_level;
	const struct rwm_draw *d = &w->draw;
	const struct rwa_texture *target = d->color_count != 0 ? d->color[0] : nullptr;
	const unsigned level = d->color_count != 0 ? d->color_level[0] : 0;

	if (rwm_pass_draws_n == 0 || target != last_target || level != last_level) {
		if (rwm_pass_draws_n == RWM_PASS_DRAWS) {
			return;
		}
		struct rwm_pass_draws *p = &rwm_pass_draws[rwm_pass_draws_n++];

		memset(p, 0, sizeof(*p));
		if (w->target_count != 0 && w->targets[0] != nullptr) {
			const struct rwa_texture_shape *shape = rwm_texture_shape(w->targets[0]);

			p->width = shape->width >> level;
			p->height = shape->height >> level;
		}
		p->level = level;
		last_target = target;
		last_level = level;
	}
	struct rwm_pass_draws *p = &rwm_pass_draws[rwm_pass_draws_n - 1];
	/* the pixels it may cover: its viewport (or the target), within its scissor */
	double x0 = 0, y0 = 0, x1 = p->width, y1 = p->height;

	if (d->viewport != nullptr) {
		x0 = d->viewport[0];
		y0 = d->viewport[1];
		x1 = x0 + d->viewport[2];
		y1 = y0 + d->viewport[3];
	}
	if (d->scissor != nullptr) {
		x0 = x0 > d->scissor[0] ? x0 : d->scissor[0];
		y0 = y0 > d->scissor[1] ? y0 : d->scissor[1];
		x1 = x1 < d->scissor[0] + d->scissor[2] ? x1 : d->scissor[0] + d->scissor[2];
		y1 = y1 < d->scissor[1] + d->scissor[3] ? y1 : d->scissor[1] + d->scissor[3];
	}
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > p->width ? p->width : x1;
	y1 = y1 > p->height ? p->height : y1;
	const uint64_t pixels = x1 > x0 && y1 > y0 ? (uint64_t)((x1 - x0) * (y1 - y0)) : 0;
	const char *label = rwm_label_of(w->pipeline);
	unsigned k;

	if (label == nullptr) {
		label = "?";
	}
	if (strstr(label, "coreanimation.") != nullptr) {
		label = strstr(label, "coreanimation.") + 14;
	}
	p->draws++;
	for (k = 0; k < p->nshaders && strncmp(p->shaders[k].label, label, sizeof(p->shaders[k].label) - 1) != 0; k++) {
	}
	if (k == p->nshaders && p->nshaders < RWM_PASS_SHADERS) {
		snprintf(p->shaders[k].label, sizeof(p->shaders[k].label), "%s", label);
		p->nshaders++;
	}
	if (k < p->nshaders) {
		p->shaders[k].draws++;
		p->shaders[k].pixels += pixels;
	} else {
		p->other_draws++;
		p->other_pixels += pixels;
	}
}

extern "C" int
rwm_run_works(struct rwa_device *device, struct rwm_work *const *works, unsigned count, bool copy, bool finish,
	      uint64_t *seq, uint64_t *waited_ns, char *why, size_t why_size)
{
	struct rwm_texture *drawn[64];
	unsigned ndrawn = 0;

	*waited_ns = 0;
	rwm_cb_now.works = count;
	for (unsigned n = 0; n < count; n++) {
		struct rwm_work *w = works[n];
		uint64_t t0 = rwm_ns();

		for (unsigned i = 0; copy && i < w->copy_in_count; i++) {
			rwm_copy_in(w->copy_in[i]);
		}
		/* (A34: the buffer's parts, "rwm worst cb") */
		const uint64_t t1 = rwm_ns();

		if (t1 - t0 > 0 && copy && w->copy_in_count != 0) {
			rwm_cb_now.copyins += w->copy_in_count;
			rwm_cb_now.copyin_ns += t1 - t0;
		}
		struct work_timer {
			const struct rwm_work *w;
			unsigned n;
			uint64_t from;

			~work_timer()
			{
				const uint64_t took = rwm_ns() - from;

				switch (w->kind) {
				case RWM_WORK_DRAW:
					rwm_cb_now.draws++;
					rwm_cb_now.draw_ns += took;
					break;
				case RWM_WORK_BLIT:
					rwm_cb_now.blits++;
					rwm_cb_now.blit_ns += took;
					break;
				case RWM_WORK_DISPATCH:
					rwm_cb_now.dispatches++;
					rwm_cb_now.dispatch_ns += took;
					break;
				}
				if (took > rwm_cb_now.slowest_ns) {
					rwm_cb_now.slowest_ns = took;
					rwm_cb_now.slowest_index = n;
					rwm_cb_now.slowest_kind = (unsigned)w->kind;
				}
			}
		} timer = { w, n, t1 };

		switch (w->kind) {
		case RWM_WORK_BLIT:
			if (rwm_run_blit(&w->blit, why, why_size) != 0) {
				static unsigned long times;

				say_again(times, "a copy skipped", why);
			} else {
				note_drawn(w, drawn, ndrawn, sizeof(drawn) / sizeof(drawn[0]));
			}
			why[0] = '\0';
			break;
		case RWM_WORK_DRAW: {
			int failed;

			rwm_perf.draws++;
			if (rwm_pass_draws_on) {
				note_pass_draw(w);
			}
			failed = rwm_run_draw(&w->draw, why, why_size);
			if (failed == -2) {
				rwm_print("the draw failed: %s\n", why);
			}
			if (failed != 0) {
				/*
				 * A buffer the shader is declared to read and the window server did not bind: QuartzCore's
				 * specialized uber shader keeps its configuration buffer (0) among its arguments although the
				 * constants decide it is never read. The draw is skipped and the rest of the frame drawn --
				 * failing the whole command buffer for it left every frame black (24 October).
				 */
				if (strstr(why, "reads buffer") != nullptr && strstr(why, "were bound") != nullptr) {
					static unsigned long times;

					say_again(times, "a draw skipped, the frame kept", why);
					why[0] = '\0';
					break;
				}
				return RWM_RUN_DRAW_FAILED;
			}
			note_drawn(w, drawn, ndrawn, sizeof(drawn) / sizeof(drawn[0]));
			break;
		}
		case RWM_WORK_DISPATCH:
			if (rwm_run_dispatch(&w->dispatch, why, why_size) != 0) {
				rwm_print("the dispatch failed: %s\n", why);
				return RWM_RUN_DISPATCH_FAILED;
			}
			break;
		}
	}

	/*
	 * Sent off and NOT waited for when the kernel queues the Mali's jobs: the window server goes on to its next frame
	 * while the GPU draws this one; the display waits for the surface's last job before it shows it, the completed
	 * handlers and -waitUntilCompleted for the buffer's. A surface drawn by the copy-back road still needs the frame
	 * now, and waits.
	 */
	const uint64_t began = rwm_ns();

	if (rwm_submit(device, ndrawn != 0 || finish, seq, why, why_size) != 0) {
		rwm_print("THE GPU DID NOT FINISH the command buffer: %s\n", why);
	}
	*waited_ns = rwm_ns() - began;
	rwm_cb_now.submit_ns = *waited_ns;
	for (unsigned k = 0; k < ndrawn; k++) {
		static unsigned long times;
		char reason[256] = { 0 };

		if (rwm_texture_sync_surface(drawn[k], reason, sizeof(reason)) != 0) {
			say_again(times, "copy-back", reason);
		}
	}
	return 0;
}
