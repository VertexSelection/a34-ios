/*
 * The compiler library, loaded with dlopen rather than linked, for a measured reason: the device
 * object must exist even when there is no GPU, and a device that refuses to be created at all takes
 * Metal's whole discovery down with it (the comment at the top of src/redwoodmetal.m).
 */
#include "rwm.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

namespace {

constexpr const char *library_path = "/a34/libgallium-26.3.0.dylib";

struct Call {
	const char *name;
	void **slot;
	bool required;
};

struct rwm_air calls;
bool loaded;

template <typename F>
Call
call(const char *name, F *slot, bool required)
{
	return Call{ name, reinterpret_cast<void **>(slot), required };
}

void
load()
{
	void *handle = dlopen(library_path, RTLD_NOW);

	if (handle == nullptr) {
		rwm_print("no compiler library: %s\n", dlerror());
		return;
	}

	/* (metal-fx) boot-arg rwm_area=strict: attachments of different sizes refused by the library, as before */
	if (rwm_boot_arg("rwm_area=strict")) {
		setenv("RWA_RENDER_AREA", "strict", 1);
	}
	/* (metal-fx) boot-arg rwm_pandebug=FLAGS: Mesa's PAN_MESA_DEBUG for the driver's own screen, before it is made
	 * (sync: every job waited for -- whether the blur's black is a read before the write; nocrc, noafbc, linear) */
	if (const char *at = strstr(rwm_boot_args(), "rwm_pandebug=")) {
		char flags[64];
		size_t n = strcspn(at + 13, " ");

		if (n > 0 && n < sizeof(flags)) {
			memcpy(flags, at + 13, n);
			flags[n] = '\0';
			setenv("PAN_MESA_DEBUG", flags, 1);
			rwm_print("PAN_MESA_DEBUG=%s (rwm_pandebug)\n", flags);
		}
	}
	/* boot-arg rwm_rebind=all: every state set for every draw and the framebuffer let go after
	 * each, as before the library kept what was bound (air_metal.c, bind_framebuffer) */
	if (rwm_boot_arg("rwm_rebind=all")) {
		setenv("RWA_REBIND_ALL", "1", 1);
	}
	struct rwm_air &c = calls;
	const Call table[] = {
		call("rwa_device_open", &c.device_open, true),
		call("rwa_device_close", &c.device_close, false),
		call("rwa_device_name", &c.device_name, false),
		call("rwa_buffer_create", &c.buffer_create, false),
		call("rwa_buffer_map", &c.buffer_map, false),
		call("rwa_buffer_size", &c.buffer_size, false),
		call("rwa_buffer_destroy", &c.buffer_destroy, false),
		call("rwa_compute_create", &c.compute_create, true),
		call("rwa_compute_destroy", &c.compute_destroy, false),
		call("rwa_texture_create", &c.texture_create, true),
		call("rwa_texture_from_buffer", &c.texture_from_buffer, false),
		call("rwa_texture_write", &c.texture_write, false),
		call("rwa_texture_read", &c.texture_read, false),
		call("rwa_texture_copy", &c.texture_copy, false),
		call("rwa_buffer_copy", &c.buffer_copy, false),
		call("rwa_buffer_fill", &c.buffer_fill, false),
		call("rwa_texture_mipmaps", &c.texture_mipmaps, false),
		call("rwa_render_clear", &c.render_clear, false),
		call("rwa_render_clear_attachments", &c.render_clear_attachments, false),
		call("rwa_texture_view", &c.texture_view, false),
		call("rwa_texture_probe", &c.texture_probe, false),
		call("rwa_depth_read", &c.depth_read, false),
		call("rwa_texture_destroy", &c.texture_destroy, false),
		call("rwa_sampler_create", &c.sampler_create, true),
		call("rwa_sampler_destroy", &c.sampler_destroy, false),
		call("rwa_dispatch", &c.dispatch, true),
		/* argument buffers, drawing and depth: looked up, not required -- the plug-in checks
		 * for them before it answers an encoder, a pipeline or a depth state */
		call("rwa_dispatch_buffers", &c.dispatch_buffers, false),
		call("rwa_module_argbufs", &c.module_argbufs, false),
		call("rwa_argbuf_encode_buffer", &c.argbuf_encode_buffer, false),
		call("rwa_argbuf_encode_texture", &c.argbuf_encode_texture, false),
		call("rwa_argbuf_encode_sampler", &c.argbuf_encode_sampler, false),
		call("rwa_argbuf_encoded_members", &c.argbuf_encoded_members, false),
		call("rwa_render_constants", &c.render_constants, false),
		call("rwa_render_create", &c.render_create, false),
		call("rwa_render_destroy", &c.render_destroy, false),
		call("rwa_render_draw", &c.render_draw, false),
		call("rwa_depth_stencil_create", &c.depth_stencil_create, false),
		call("rwa_depth_stencil_destroy", &c.depth_stencil_destroy, false),
		call("rwa_finish", &c.finish, false),
		call("rwa_flush_seq", &c.flush_seq, false),
		call("rwa_wait_seq", &c.wait_seq, false),
		call("rwa_surface_seq", &c.surface_seq, false),
	};

	for (const Call &entry : table) {
		*entry.slot = dlsym(handle, entry.name);
		if (entry.required && *entry.slot == nullptr) {
			/* rwa_dispatch's arguments changed when textures were added to the contract, so
			 * an older library would be called with the wrong shape of argument and do
			 * something arbitrary: refusing it outright is the only safe reading */
			rwm_print("the compiler library has no %s, so it is older than this driver and "
				  "will not be used\n", entry.name);
			calls = {};
			dlclose(handle);
			return;
		}
	}
	c.handle = handle;
	loaded = true;
}

std::once_flag once;

} // namespace

extern "C" bool
rwm_air_available(void)
{
	std::call_once(once, load);
	return loaded;
}

extern "C" const struct rwm_air *
rwm_air(void)
{
	std::call_once(once, load);
	return &calls;
}
