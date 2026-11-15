/*
 * A draw, run when its command buffer is committed: Metal's tables made into the library's rwa_draw
 * (tools/mesa/air/air_metal.h) and drawn.
 */
#include "rwm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static_assert(RWM_MAX_BUFFERS <= 32, "the stream mask is 32 bits");

/*
 * (metal-fx) Boot-arg rwm_blurdump=N: for the first N draws into a level below the first of a texture (a blur's
 * smaller copies, CoreAnimation's pyramid), the texel at the centre of that level and of the first, after the draw
 * (waits for the GPU: those draws are slow). Lines "blurdump:".
 */
static long blur_dumps_left = -1;

extern "C" bool
rwm_blurdump_on(void)
{
	if (blur_dumps_left < 0) {
		const char *at = strstr(rwm_boot_args(), "rwm_blurdump=");

		blur_dumps_left = at != nullptr ? atol(at + 13) : 0;
	}
	return blur_dumps_left > 0;
}

/* " WxH bytes" of the texel at the centre of `level` (and `slice`) of a texture, appended to `line` */
static size_t
probe_into(char *line, size_t used, size_t size, struct rwa_texture *texture, unsigned level, unsigned slice)
{
	uint8_t texel[16] = {};
	unsigned w = 0, h = 0;
	char why[120] = {};
	int bytes = rwm_air()->texture_probe(texture, level, slice, texel, &w, &h, why, sizeof(why));

	if (bytes <= 0) {
		return used + (size_t)snprintf(line + used, size - used, " (%s)", why);
	}
	used += (size_t)snprintf(line + used, size - used, " %ux%u", w, h);
	for (int b = 0; b < bytes && b < 8 && used < size - 4; b++) {
		used += (size_t)snprintf(line + used, size - used, "%s%02x", b == 0 ? " " : "", texel[b]);
	}
	return used;
}

extern "C" void
rwm_blurdump_used(void)
{
	if (blur_dumps_left > 0) {
		blur_dumps_left--;
	}
}

static void
blur_dump(const struct rwm_draw *d)
{
	const unsigned level = d->color_levels ? d->color_level[0] : 0;

	/* a draw into a level below the first (a pyramid), or one of a blur's or a material's pipelines (any target) */
	if (!rwm_blurdump_on() || d->color_count == 0 || d->color[0] == nullptr || (level == 0 && d->probe[0] == '\0') ||
	    rwm_air()->texture_probe == nullptr) {
		return;
	}
	blur_dumps_left--;

	char line[400];
	size_t used = 0;

	/* what it drew: the centre of every attachment (the second is CoreAnimation's clip mask) */
	used += (size_t)snprintf(line + used, sizeof(line) - used, "blurdump: %s into level %u:", d->probe[0] ? d->probe
												: "a pass", level);
	for (unsigned i = 0; i < d->color_count && i < 3 && used < sizeof(line) - 80; i++) {
		if (d->color[i] == nullptr) {
			continue;
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, "%s a%u", i ? ";" : "", i);
		used = probe_into(line, used, sizeof(line), d->color[i], d->color_levels ? d->color_level[i] : 0,
				  d->color_levels ? d->color_slice[i] : 0);
	}
	rwm_print("%s\n", line);

	/* and what it read: the centre of level 0 of every texture bound for the fragment stage */
	used = (size_t)snprintf(line, sizeof(line), "blurdump:   reads");
	for (unsigned i = 0, n = 0; i < d->texture_count && i < RWM_MAX_BUFFERS && n < 6 && used < sizeof(line) - 80; i++) {
		if (d->textures[i] == nullptr) {
			continue;
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, "%s t%u", n++ ? ";" : "", i);
		used = probe_into(line, used, sizeof(line), d->textures[i], 0, 0);
	}
	rwm_print("%s\n", line);
}

extern "C" int
rwm_run_draw(const struct rwm_draw *d, char *why, size_t why_size)
{
	const struct rwm_air *air = rwm_air();
	struct rwa_buffer *vertex_buffers[RWM_MAX_BUFFERS] = {};
	struct rwa_vertex_stream vertex_streams[RWM_MAX_BUFFERS] = {};
	/* and Metal's own table for each stage: the same entries with the OFFSET beside each, which
	 * rwa_bindings has nowhere to put. One array for both of the things an offset means -- which
	 * encoding of an argument buffer, and which block of a buffer argument -- because Metal has one
	 * call behind both. */
	struct rwa_buffer_bind vertex_table[RWM_MAX_BUFFERS] = {};
	struct rwa_buffer_bind fragment_table[RWM_MAX_BUFFERS] = {};
	const unsigned vertex_count = d->vertex_count_bound < RWM_MAX_BUFFERS ? d->vertex_count_bound : RWM_MAX_BUFFERS;
	const unsigned fragment_count = d->fragment_count_bound < RWM_MAX_BUFFERS ? d->fragment_count_bound
										  : RWM_MAX_BUFFERS;

	if (air->render_draw == nullptr) {
		snprintf(why, why_size, "this compiler library can compute but not yet draw");
		return -1;
	}
	/*
	 * Metal's one vertex buffer table, split in two by the pipeline's own mask: an index the vertex
	 * descriptor fetches attributes from is a STREAM, with its offset, and every other index is a
	 * [[buffer(n)]] ARGUMENT of the vertex stage. The library refuses a descriptor that claims an index
	 * the module also reads as a buffer (air_metal.h), so the two sets cannot overlap and this split
	 * cannot drop a binding.
	 */
	for (unsigned i = 0; i < vertex_count; i++) {
		const bool stream = (d->stream_mask & (1u << i)) != 0;

		/* Metal's own offset, for whichever of the other two things index i is: an argument buffer
		 * (which encoding to read) or a buffer argument (which block); a stream's offset is the
		 * stream array's, and an index is never both */
		if (!stream) {
			vertex_table[i].buffer = d->vertex[i];
			vertex_table[i].offset = d->vertex_offset[i];
		}
		if (i < RWA_MAX_VERTEX_BUFFERS && stream) {
			vertex_streams[i].buffer = d->vertex[i];
			vertex_streams[i].offset = d->vertex_offset[i];
			if (d->vertex_bound[i] && d->vertex[i] == nullptr) {
				snprintf(why, why_size, "the buffer bound as vertex stream %u did not come from the "
					 "compiler library, so there is nothing to fetch vertices out of", i);
				return -1;
			}
			continue;
		}
		vertex_buffers[i] = d->vertex[i];
	}
	if (d->stream_count > RWM_MAX_BUFFERS) {
		snprintf(why, why_size, "this pipeline fetches from %u vertex stream slots and this driver's table "
			 "holds %u", d->stream_count, RWM_MAX_BUFFERS);
		return -1;
	}
	for (unsigned i = 0; i < fragment_count; i++) {
		fragment_table[i].buffer = d->fragment[i];
		fragment_table[i].offset = d->fragment_offset[i];
		if (d->fragment_bound[i] && d->fragment[i] == nullptr) {
			snprintf(why, why_size, "the buffer bound at fragment buffer index %u did not come from the "
				 "compiler library, so there is nothing to bind", i);
			return -1;
		}
	}
	/*
	 * The index buffer, by the same rule as every other resource here: it has to be one of the
	 * library's, because what the library needs is the Gallium resource behind it. NULL here would be
	 * a draw the library reads as non-indexed -- the vertices in the wrong order, which is the one
	 * outcome worse than an error -- so it is caught here and not left to the library.
	 */
	if (d->index_bound && d->index_buffer == nullptr) {
		snprintf(why, why_size, "the index buffer of this draw did not come from the compiler library, so "
			 "there is nothing to read the indices out of");
		return -1;
	}

	struct rwa_draw draw = {};

	draw.color = d->color;
	draw.color_count = d->color_count;
	draw.clear_color = d->clear_color;
	/* (metal-fx) each attachment's level and slice, and its own load action */
	draw.color_level = d->color_levels ? d->color_level : nullptr;
	draw.color_slice = d->color_levels ? d->color_slice : nullptr;
	draw.clear_color_mask = d->clear_color_mask;
	draw.clear_colors = &d->clear_colors[0][0];
	/* the depth attachment, its two load actions, the state that was bound, and the depth range of the
	 * viewport -- where the quad's own depth comes from (without one, the contract's [0, 1]) */
	draw.depth = d->depth;
	draw.clear_depth = d->clear_depth;
	draw.clear_stencil = d->clear_stencil;
	draw.depth_stencil = d->depth_stencil;
	draw.stencil_reference[0] = d->stencil_reference[0];
	draw.stencil_reference[1] = d->stencil_reference[1];
	draw.depth_range = d->depth_range;
	draw.viewport = d->viewport;
	draw.scissor = d->scissor;
	draw.vertex.buffers = vertex_buffers;
	draw.vertex.buffer_count = vertex_count;
	/* the streams the layout fetches from, at Metal's own indices with Metal's own offsets; the
	 * count is the highest index the pipeline fetches from plus one */
	draw.vertex_streams = vertex_streams;
	draw.vertex_stream_count = d->stream_count;
	draw.fragment.buffers = const_cast<struct rwa_buffer **>(d->fragment);
	draw.fragment.buffer_count = fragment_count;
	draw.fragment.textures = const_cast<struct rwa_texture **>(d->textures);
	draw.fragment.texture_count = d->texture_count;
	draw.fragment.samplers = const_cast<struct rwa_sampler **>(d->samplers);
	draw.fragment.sampler_count = d->sampler_count;
	draw.primitive = d->primitive;
	draw.vertex_start = d->vertex_start;
	draw.vertex_count = d->vertex_count;
	draw.index_buffer = d->index_buffer;
	draw.index_type = d->index_type;
	draw.index_buffer_offset = d->index_offset;
	draw.base_vertex = d->base_vertex;
	draw.instance_count = d->instance_count;
	draw.base_instance = d->base_instance;
	/* Metal's own table for each stage, at Metal's own indices: the library takes an argument buffer's
	 * whole binding from here and a buffer argument's offset only, and checks that where both name a
	 * buffer they name the same one */
	draw.vertex_buffer_table = vertex_table;
	draw.vertex_buffer_table_count = vertex_count;
	draw.fragment_buffer_table = fragment_table;
	draw.fragment_buffer_table_count = fragment_count;

	if (air->render_draw(d->render, &draw, why, why_size) != 0) {
		return -2;
	}
	blur_dump(d);
	return 0;
}
