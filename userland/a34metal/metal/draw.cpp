/*
 * A draw, run when its command buffer is committed: Metal's tables made into the library's rwa_draw
 * (tools/mesa/air/air_metal.h) and drawn.
 */
#include "rwm.h"

#include <stdint.h>
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

/*
 * A summary of sixteen texels on a 4 x 4 grid over a rectangle of a level of a texture: how many are opaque
 * (alpha > 0xf0), how many are nothing (all zero), and the mean colour, appended to `line` as
 * " #id WxH [x,y wxh] o/z mean bbggrraa". `#id` tells the textures of one frame apart (the same number is the same
 * texture); a rectangle of 0 x 0 is the whole level.
 */
static size_t
probe_into(char *line, size_t used, size_t size, struct rwa_texture *texture, unsigned level, unsigned slice,
	   const unsigned box_in[4])
{
	uint8_t texel[16] = {};
	unsigned w = 0, h = 0, opaque = 0, empty = 0, sum[4] = {}, got = 0, bytes = 0;
	char why[120] = {};
	const struct rwm_air *air = rwm_air();
	int first = air->texture_probe(texture, level, slice, texel, &w, &h, why, sizeof(why));

	if (first <= 0) {
		return used + (size_t)snprintf(line + used, size - used, " #%04x (%s)",
					       (unsigned)((uintptr_t)texture >> 4 & 0xffff), why);
	}
	bytes = (unsigned)first;
	unsigned box[4] = { 0, 0, w, h };

	if (box_in != nullptr && box_in[2] != 0 && box_in[3] != 0) {
		box[0] = box_in[0] < w ? box_in[0] : w - 1;
		box[1] = box_in[1] < h ? box_in[1] : h - 1;
		box[2] = box_in[2] < w - box[0] ? box_in[2] : w - box[0];
		box[3] = box_in[3] < h - box[1] ? box_in[3] : h - box[1];
	}
	for (unsigned gy = 0; gy < 4 && air->texture_probe_at != nullptr; gy++) {
		for (unsigned gx = 0; gx < 4; gx++) {
			unsigned x = box[0] + (2 * gx + 1) * box[2] / 8, y = box[1] + (2 * gy + 1) * box[3] / 8;

			if (air->texture_probe_at(texture, level, slice, x, y, texel, &w, &h, why, sizeof(why)) <= 0) {
				continue;
			}
			got++;
			opaque += bytes >= 4 && texel[3] > 0xf0;
			empty += texel[0] == 0 && texel[1] == 0 && (bytes < 3 || texel[2] == 0) && (bytes < 4 || texel[3] == 0);
			for (unsigned c = 0; c < 4 && c < bytes; c++) {
				sum[c] += texel[c];
			}
		}
	}
	used += (size_t)snprintf(line + used, size - used, " #%04x %ux%u [%u,%u %ux%u] %u/%u", (unsigned)((uintptr_t)texture >> 4 & 0xffff),
				 w, h, box[0], box[1], box[2], box[3], opaque, empty);
	for (unsigned c = 0; c < 4 && c < bytes && got != 0; c++) {
		used += (size_t)snprintf(line + used, size - used, "%s%02x", c == 0 ? " " : "", sum[c] / got);
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

/*
 * (metal-fx) Boot-arg rwm_framedump=N: once a blur has been drawn (a downsample, a narrow blur, a variable blur), the
 * next N draws into a target of the screen's size (whatever their pipeline), a line each ("framedump:"): the pipeline,
 * the target (#id), whether the draw cleared it, and a 4 x 4 grid over its whole first level after the draw. Boot
 * 0938-metal120-d: the target the blur samples and composites into held black and the materials' tint only, while
 * the screen showed the wallpaper -- this shows into which target the wallpaper and the icons are drawn, and whether
 * anything clears that target in between.
 */
static long frame_dumps_left = -1;
static bool blur_seen;

extern "C" bool
rwm_framedump_on(void)
{
	if (frame_dumps_left < 0) {
		const char *at = strstr(rwm_boot_args(), "rwm_framedump=");

		frame_dumps_left = at != nullptr ? atol(at + 14) : 0;
	}
	return frame_dumps_left > 0;
}

static size_t probe_into(char *line, size_t used, size_t size, struct rwa_texture *texture, unsigned level,
			 unsigned slice, const unsigned box_in[4]);

static void
frame_dump(const struct rwm_draw *d)
{
	if (!rwm_framedump_on() || d->color_count == 0 || d->color[0] == nullptr || rwm_air()->texture_probe == nullptr) {
		return;
	}
	if (!blur_seen) {
		blur_seen = strstr(d->label, "_Tds") != nullptr || strstr(d->label, "_Tn1") != nullptr ||
			    strstr(d->label, "_Tn2") != nullptr || strstr(d->label, "_Tvr") != nullptr;
		if (!blur_seen) {
			return;
		}
	}

	uint8_t texel[16];
	unsigned w = 0, h = 0;
	char why[64], line[400];

	if (rwm_air()->texture_probe(d->color[0], 0, 0, texel, &w, &h, why, sizeof(why)) <= 0 || w < 1000) {
		return;
	}
	frame_dumps_left--;

	size_t used = (size_t)snprintf(line, sizeof(line), "framedump: %s%s%s:", d->label[0] ? d->label : "?",
				       (d->clear_color_mask & 1) != 0 ? " (clears a0)" : "",
				       d->clear_color != nullptr ? " (clears)" : "");

	used = probe_into(line, used, sizeof(line), d->color[0], d->color_levels ? d->color_level[0] : 0, 0, nullptr);
	if (d->scissor != nullptr && used < sizeof(line) - 40) {
		used += (size_t)snprintf(line + used, sizeof(line) - used, " scissor %u,%u %ux%u", d->scissor[0], d->scissor[1],
					 d->scissor[2], d->scissor[3]);
	}
	rwm_print("%s\n", line);
}

static void
blur_dump(const struct rwm_draw *d)
{
	const unsigned level = d->color_levels ? d->color_level[0] : 0;
	static unsigned lub_dumps;

	/* a draw into a level below the first (a pyramid), or one of a blur's or a material's pipelines (any target) */
	if (!rwm_blurdump_on() || d->color_count == 0 || d->color[0] == nullptr || (level == 0 && d->probe[0] == '\0') ||
	    rwm_air()->texture_probe == nullptr) {
		return;
	}
	/* (the luma boost runs over every frame: two of it, and the budget kept for the blurs) */
	if (strstr(d->probe, "_Tlub") != nullptr && lub_dumps++ >= 2) {
		return;
	}
	blur_dumps_left--;

	char line[480];
	size_t used = 0;
	/* where it draws: the scissor, else the viewport, else the whole attachment */
	unsigned area[4] = {};

	if (d->scissor != nullptr) {
		memcpy(area, d->scissor, sizeof(area));
	} else if (d->viewport != nullptr) {
		area[0] = (unsigned)d->viewport[0];
		area[1] = (unsigned)d->viewport[1];
		area[2] = (unsigned)d->viewport[2];
		area[3] = (unsigned)d->viewport[3];
	}
	/* what it drew, over where it draws: every attachment (the second is CoreAnimation's clip mask); "o/z" are how
	 * many of 16 texels are opaque and how many all zero, then the mean B G R A */
	used += (size_t)snprintf(line + used, sizeof(line) - used, "blurdump: %s into level %u:", d->probe[0] ? d->probe
												: "a pass", level);
	for (unsigned i = 0; i < d->color_count && i < 3 && used < sizeof(line) - 100; i++) {
		if (d->color[i] == nullptr) {
			continue;
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, "%s a%u", i ? ";" : "", i);
		used = probe_into(line, used, sizeof(line), d->color[i], d->color_levels ? d->color_level[i] : 0,
				  d->color_levels ? d->color_slice[i] : 0, area);
	}
	rwm_print("%s\n", line);

	/* and what it read: level 0 of every texture bound for the fragment stage, the whole of it */
	used = (size_t)snprintf(line, sizeof(line), "blurdump:   reads");
	for (unsigned i = 0, n = 0; i < d->texture_count && i < RWM_MAX_BUFFERS && n < 5 && used < sizeof(line) - 100; i++) {
		if (d->textures[i] == nullptr) {
			continue;
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, "%s t%u", n++ ? ";" : "", i);
		used = probe_into(line, used, sizeof(line), d->textures[i], 0, 0, nullptr);
	}
	rwm_print("%s\n", line);
}

/*
 * (metal-fx) Boot-arg rwm_scene=N: what CoreAnimation draws, for N windows of five seconds, and whether the draws of
 * the kinds behind the missing pictures draw anything. Lines "scene:".
 *
 *  - every window: each pipeline drawn with, and how many times ("scene 5 s: ... label xN; ...");
 *  - the first two draws of a window of each pipeline that clips (Ccl), draws into the second attachment (A2S1),
 *    reads an attachment (M0, M1), blends programmably or by another rule than source-over (Badd, Bpld, Bcbn, Bvcm,
 *    Bdin, Bdot, Bdov, Bdso), samples one channel (Ired, Igrn, Ialp), clears (Tatc) or is a step of a blur of a small
 *    radius (_Tvr, _Tsb, _Tbds, _Tdb, _Tups, _Thom, _Tnb), 400 in all: the attachment it
 *    draws into over where it draws (its scissor, else its viewport) before and after it ("o/z": of 16 texels on a
 *    4 x 4 grid, how many opaque and how many all zero; the mean B G R A; how many changed), the second attachment
 *    there when it reads that, and the textures it reads (whole; "surface N": laid over IOSurface N, "copied N":
 *    copied in from it);
 *  - once a window, each surface of 600 pixels or wider that a draw reads or draws into, and what it holds.
 *
 * The probes wait for the GPU, and the two around a draw cut its pass in three: the draws probed run as if each were
 * a pass of its own.
 */
static long scene_windows_left = -1;

extern "C" bool
rwm_scene_on(void)
{
	if (scene_windows_left < 0) {
		const char *at = strstr(rwm_boot_args(), "rwm_scene=");

		scene_windows_left = at != nullptr ? atol(at + 10) : 0;
	}
	return scene_windows_left > 0;
}

namespace {

struct scene_label {
	char label[64];
	unsigned draws, probed;
};

scene_label scene_labels[160];
unsigned scene_label_count, scene_draws, scene_window;
uint64_t scene_began;
unsigned scene_probes_left = 400;
uint32_t scene_surfaces[48];
unsigned scene_surface_count;

struct scene_grid {
	uint8_t texel[16][4];
	bool got[16];
	unsigned bytes, w, h, box[4];
};

struct scene_probe {
	unsigned dst, area[4];
	bool scissor;
	scene_grid before;
	const char *label;
};

const char *
scene_name(const struct rwm_draw *d)
{
	const char *label = d->label[0] ? d->label : "(none)";

	return strncmp(label, "draw.", 5) == 0 ? label + 5 : label;
}

bool
scene_wanted(const char *label)
{
	static const char *const keys[] = { "Ccl", "A2S1", "M0", "M1", "Badd", "Bpld", "Bcbn", "Bvcm", "Bdin", "Bdot",
					    "Bdov", "Bdso", "Ired", "Igrn", "Ialp", "Tatc", "_Tvr", "_Tsb", "_Tbds", "_Tdb",
					    "_Tups", "_Thom", "_Tnb" };

	for (const char *k : keys) {
		if (strstr(label, k) != nullptr) {
			return true;
		}
	}
	return false;
}

/* the window's pipelines, most drawn first, in lines a log line holds */
void
scene_window_end(void)
{
	char line[440];
	size_t used = (size_t)snprintf(line, sizeof(line), "scene 5 s (%u): %u draws:", ++scene_window, scene_draws);
	bool taken[160] = {};

	for (unsigned n = 0; n < scene_label_count; n++) {
		unsigned best = 0;
		bool any = false;

		for (unsigned i = 0; i < scene_label_count; i++) {
			if (!taken[i] && (!any || scene_labels[i].draws > scene_labels[best].draws)) {
				best = i;
				any = true;
			}
		}
		taken[best] = true;

		char one[96];
		const int len = snprintf(one, sizeof(one), " %s x%u;", scene_labels[best].label, scene_labels[best].draws);

		if (used + (size_t)len >= sizeof(line) - 1) {
			rwm_print("%s\n", line);
			used = (size_t)snprintf(line, sizeof(line), "scene 5 s (%u), more:", scene_window);
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, "%s", one);
	}
	rwm_print("%s\n", line);
	scene_label_count = 0;
	scene_draws = 0;
	scene_surface_count = 0;
	scene_windows_left--;
}

scene_label *
scene_count(const char *label)
{
	for (unsigned i = 0; i < scene_label_count; i++) {
		if (strcmp(scene_labels[i].label, label) == 0) {
			return &scene_labels[i];
		}
	}
	if (scene_label_count == sizeof(scene_labels) / sizeof(scene_labels[0])) {
		return nullptr;
	}
	scene_label *made = &scene_labels[scene_label_count++];

	snprintf(made->label, sizeof(made->label), "%s", label);
	made->draws = 0;
	made->probed = 0;
	return made;
}

/* sixteen texels on a 4 x 4 grid over `area` of a level (all of it when the area is empty) */
bool
scene_sample(struct rwa_texture *texture, unsigned level, unsigned slice, const unsigned area[4], scene_grid &g)
{
	const struct rwm_air *air = rwm_air();
	uint8_t texel[16] = {};
	char why[96];

	memset(&g, 0, sizeof(g));
	if (texture == nullptr || air->texture_probe == nullptr || air->texture_probe_at == nullptr) {
		return false;
	}
	const int bytes = air->texture_probe(texture, level, slice, texel, &g.w, &g.h, why, sizeof(why));

	if (bytes <= 0 || g.w == 0 || g.h == 0) {
		return false;
	}
	g.bytes = (unsigned)bytes;
	g.box[0] = 0;
	g.box[1] = 0;
	g.box[2] = g.w;
	g.box[3] = g.h;
	if (area != nullptr && area[2] != 0 && area[3] != 0) {
		g.box[0] = area[0] < g.w ? area[0] : g.w - 1;
		g.box[1] = area[1] < g.h ? area[1] : g.h - 1;
		g.box[2] = area[2] < g.w - g.box[0] ? area[2] : g.w - g.box[0];
		g.box[3] = area[3] < g.h - g.box[1] ? area[3] : g.h - g.box[1];
	}
	for (unsigned gy = 0; gy < 4; gy++) {
		for (unsigned gx = 0; gx < 4; gx++) {
			const unsigned x = g.box[0] + (2 * gx + 1) * g.box[2] / 8, y = g.box[1] + (2 * gy + 1) * g.box[3] / 8;
			unsigned w = 0, h = 0;

			if (air->texture_probe_at(texture, level, slice, x, y, texel, &w, &h, why, sizeof(why)) > 0) {
				memcpy(g.texel[gy * 4 + gx], texel, 4);
				g.got[gy * 4 + gx] = true;
			}
		}
	}
	return true;
}

/* " o/z mean" of a grid */
size_t
scene_say(char *line, size_t used, size_t size, const scene_grid &g)
{
	unsigned opaque = 0, empty = 0, got = 0, sum[4] = {};

	for (unsigned i = 0; i < 16; i++) {
		if (!g.got[i]) {
			continue;
		}
		const uint8_t *t = g.texel[i];

		got++;
		opaque += g.bytes >= 4 && t[3] > 0xf0;
		empty += t[0] == 0 && (g.bytes < 2 || t[1] == 0) && (g.bytes < 3 || t[2] == 0) && (g.bytes < 4 || t[3] == 0);
		for (unsigned c = 0; c < 4 && c < g.bytes; c++) {
			sum[c] += t[c];
		}
	}
	used += (size_t)snprintf(line + used, size - used, " %u/%u", opaque, empty);
	for (unsigned c = 0; c < 4 && c < g.bytes && got != 0; c++) {
		used += (size_t)snprintf(line + used, size - used, "%s%02x", c == 0 ? " " : "", sum[c] / got);
	}
	return used;
}

unsigned
scene_changed(const scene_grid &a, const scene_grid &b)
{
	unsigned n = 0;

	for (unsigned i = 0; i < 16; i++) {
		n += a.got[i] && b.got[i] && memcmp(a.texel[i], b.texel[i], a.bytes < 4 ? a.bytes : 4) != 0;
	}
	return n;
}

bool
scene_seen(uint32_t surface)
{
	for (unsigned i = 0; i < scene_surface_count; i++) {
		if (scene_surfaces[i] == surface) {
			return true;
		}
	}
	if (scene_surface_count == sizeof(scene_surfaces) / sizeof(scene_surfaces[0])) {
		return true;    /* (the list full: none said until the next window) */
	}
	scene_surfaces[scene_surface_count++] = surface;
	return false;
}

/* before the draw: counted, and when it is one to probe, its target sampled */
bool
scene_before(const struct rwm_draw *d, scene_probe &p)
{
	if (!rwm_scene_on()) {
		return false;
	}
	const uint64_t now = rwm_ns();

	if (scene_began == 0) {
		scene_began = now;
	} else if (now - scene_began > 5000000000ull) {
		scene_window_end();
		scene_began = now;
		if (!rwm_scene_on()) {
			return false;
		}
	}
	p.label = scene_name(d);
	scene_draws++;

	scene_label *l = scene_count(p.label);

	if (l == nullptr) {
		return false;
	}
	l->draws++;
	if (!scene_wanted(p.label) || l->probed >= 2 || scene_probes_left == 0 || d->color_count == 0) {
		return false;
	}
	p.dst = strstr(p.label, "A2S1") != nullptr && d->color_count > 1 ? 1 : 0;
	if (d->color[p.dst] == nullptr) {
		return false;
	}
	l->probed++;
	scene_probes_left--;
	memset(p.area, 0, sizeof(p.area));
	p.scissor = d->scissor != nullptr;
	if (d->scissor != nullptr) {
		memcpy(p.area, d->scissor, sizeof(p.area));
	} else if (d->viewport != nullptr) {
		p.area[0] = d->viewport[0] > 0 ? (unsigned)d->viewport[0] : 0;
		p.area[1] = d->viewport[1] > 0 ? (unsigned)d->viewport[1] : 0;
		p.area[2] = d->viewport[2] > 0 ? (unsigned)d->viewport[2] : 0;
		p.area[3] = d->viewport[3] > 0 ? (unsigned)d->viewport[3] : 0;
	}
	return scene_sample(d->color[p.dst], d->color_levels ? d->color_level[p.dst] : 0,
			    d->color_levels ? d->color_slice[p.dst] : 0, p.area, p.before);
}

void
scene_after(const struct rwm_draw *d, const scene_probe &p)
{
	scene_grid after;
	char line[440];
	size_t used;

	if (!scene_sample(d->color[p.dst], d->color_levels ? d->color_level[p.dst] : 0,
			  d->color_levels ? d->color_slice[p.dst] : 0, p.area, after)) {
		return;
	}
	used = (size_t)snprintf(line, sizeof(line), "scene: %s a%u #%04x%s %ux%u [%u,%u %ux%u] %s%s: before", p.label,
				p.dst, (unsigned)((uintptr_t)d->color[p.dst] >> 4 & 0xffff),
				d->clear_color != nullptr || (d->clear_color_mask & (1u << p.dst)) ? " (cleared)" : "",
				after.w, after.h, after.box[0], after.box[1], after.box[2], after.box[3],
				p.scissor ? "scissor" : "viewport", d->color_surface[p.dst] ? " (a surface)" : "");
	used = scene_say(line, used, sizeof(line), p.before);
	used += (size_t)snprintf(line + used, sizeof(line) - used, ", after");
	used = scene_say(line, used, sizeof(line), after);
	used += (size_t)snprintf(line + used, sizeof(line) - used, ", %u/16 changed", scene_changed(p.before, after));
	/* the second attachment there, for a draw that reads it (a clip, a group drawn there) */
	if (p.dst == 0 && d->color_count > 1 && d->color[1] != nullptr &&
	    (strstr(p.label, "Ccl") != nullptr || strstr(p.label, "M1") != nullptr)) {
		scene_grid second;

		if (scene_sample(d->color[1], d->color_levels ? d->color_level[1] : 0, d->color_levels ? d->color_slice[1] : 0,
				 p.area, second)) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, "; a1 there");
			used = scene_say(line, used, sizeof(line), second);
		}
	}
	rwm_print("%s\n", line);

	used = (size_t)snprintf(line, sizeof(line), "scene:   reads");
	for (unsigned i = 0, n = 0; i < d->texture_count && i < RWM_MAX_BUFFERS && n < 3 && used < sizeof(line) - 120; i++) {
		scene_grid g;

		if (d->textures[i] == nullptr || !scene_sample(d->textures[i], 0, 0, nullptr, g)) {
			continue;
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, "%s t%u #%04x %ux%u %ub", n++ ? ";" : "", i,
					 (unsigned)((uintptr_t)d->textures[i] >> 4 & 0xffff), g.w, g.h, g.bytes);
		if (d->texture_surface[i] != 0) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " (%s %u)",
						 d->texture_surface[i] & 0x80000000u ? "copied" : "surface",
						 d->texture_surface[i] & 0x7fffffffu);
		}
		used = scene_say(line, used, sizeof(line), g);
	}
	rwm_print("%s\n", line);
}

/* once a window, each wide surface read or drawn into, and what it holds */
void
scene_surfaces_of(const struct rwm_draw *d)
{
	if (!rwm_scene_on()) {
		return;
	}
	for (unsigned i = 0; i < d->texture_count && i < RWM_MAX_BUFFERS; i++) {
		const uint32_t id = d->texture_surface[i];
		scene_grid g;

		if (id == 0 || d->textures[i] == nullptr || scene_seen(id) || !scene_sample(d->textures[i], 0, 0, nullptr, g) || g.w < 600) {
			continue;
		}
		char line[300];
		size_t used = (size_t)snprintf(line, sizeof(line), "scene: %s reads %s %u (t%u) %ux%u:", scene_name(d),
					       id & 0x80000000u ? "copied" : "surface", id & 0x7fffffffu, i, g.w, g.h);

		used = scene_say(line, used, sizeof(line), g);
		rwm_print("%s\n", line);
	}
	for (unsigned i = 0; i < d->color_count && i < 8; i++) {
		const uint32_t id = d->color_surface[i];
		scene_grid g;

		if (id == 0 || d->color[i] == nullptr || scene_seen(id | 0x40000000u) ||
		    !scene_sample(d->color[i], 0, 0, nullptr, g) || g.w < 600) {
			continue;
		}
		char line[300];
		size_t used = (size_t)snprintf(line, sizeof(line), "scene: %s draws into surface %u (a%u) %ux%u, after:",
					       scene_name(d), id, i, g.w, g.h);

		used = scene_say(line, used, sizeof(line), g);
		rwm_print("%s\n", line);
	}
}

} // namespace

/* (metal-fx) rwm_scene: a texture-to-texture copy of 600 pixels or wider (a snapshot, a backdrop), the first 60: what
 * the source holds and what the destination holds after */
extern "C" void
rwm_scene_copy(struct rwa_texture *from, unsigned from_level, struct rwa_texture *to, unsigned to_level,
	       const unsigned size[3])
{
	static unsigned said;
	scene_grid a, b;
	char line[300];

	if (!rwm_scene_on() || size[0] < 600 || said >= 60 || !scene_sample(from, from_level, 0, nullptr, a) ||
	    !scene_sample(to, to_level, 0, nullptr, b)) {
		return;
	}
	said++;
	size_t used = (size_t)snprintf(line, sizeof(line), "scene: copy %ux%u from #%04x", size[0], size[1],
				       (unsigned)((uintptr_t)from >> 4 & 0xffff));

	used = scene_say(line, used, sizeof(line), a);
	used += (size_t)snprintf(line + used, sizeof(line) - used, " into #%04x, after:", (unsigned)((uintptr_t)to >> 4 & 0xffff));
	used = scene_say(line, used, sizeof(line), b);
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
	draw.load_dontcare_mask = d->load_dontcare_mask;
	draw.store_dontcare_mask = d->store_dontcare_mask;
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

	scene_probe probe;
	const bool probing = scene_before(d, probe);

	if (air->render_draw(d->render, &draw, why, why_size) != 0) {
		return -2;
	}
	if (probing) {
		scene_after(d, probe);
	}
	scene_surfaces_of(d);
	blur_dump(d);
	frame_dump(d);
	return 0;
}
