/*
 * The OpenGL ES library behind eagl.c: Mesa's panfrost (Mali-G68) as userland/mesa/build.sh
 * builds it, reached through EGL on the surfaceless platform.
 *
 * This file is all that knows which library draws. eagl.c wants, besides the gl* functions
 * themselves: a context of a version of OpenGL ES, that context made the calling thread's, a
 * context taken away again, a name for the log, and a texture made of an IOSurface's memory.
 * It takes the place of ios-topaz's gl_osmesa.c (OSMesa with an ES profile, freedreno): OSMesa
 * is gone from Mesa 26, and EGL is the way to a GPU's contexts without a window system.
 *
 *   display    one, for the process: EGL_PLATFORM_SURFACELESS_MESA (the device list comes from
 *              redwood-drm's drmGetDevices2: the RedwoodMali service), OpenGL ES bound.
 *   context    OpenGL ES 3 for any API CoreAnimation asks for (eagl.c's own programs are of
 *              GLSL ES 3.00; ES 3 does all ES 2 does), with a 16 x 16 pbuffer to be current
 *              with -- nothing is drawn into it: CoreAnimation draws into framebuffers of its
 *              own, whose textures are IOSurfaces (eagl.c).
 *   IOSurface  eglCreateImageKHR(EGL_IOSURFACE_A34, the surface's ID) -- userland/mesa/patches/
 *              0003-*: the kernel describes the surface and hands its memory over as a GEM
 *              handle (RedwoodMali selector 0x7e), Mesa imports that as a dma-buf would be
 *              (pipe_screen::resource_from_handle) -- then glEGLImageTargetTexture2DOES. The
 *              image is let go at once: the texture holds the memory for as long as it lives.
 *
 * EGL lets a context be current on one thread at a time. When a thread cannot have the
 * context because another still has it, a34gl_make_current() fails and says so, and
 * a34gl_contended() is true from then on (eagl.c then lets the context go at the end of
 * every frame).
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EGL_NO_X11 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglmesaext.h>

#include "a34gl.h"

#ifndef EGL_IOSURFACE_A34
#error "EGL/eglmesaext.h of userland/mesa (patches/0003-*) is wanted: EGL_IOSURFACE_A34"
#endif

/* (the few OpenGL ES names used here; the library's own header is not wanted next to eagl.c's) */
#define GL_TEXTURE_2D           0x0de1
#define GL_RENDERER             0x1f01
#define GL_NO_ERROR             0
extern unsigned glGetError(void);
extern const unsigned char *glGetString(unsigned name);

#define WINDOW  16

struct a34gl {
	EGLContext context;
	EGLSurface surface;
	int version;
};

static pthread_once_t display_once = PTHREAD_ONCE_INIT;
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLConfig config;
static EGLint config_apis;
static bool images;                     /* EGL_IOSURFACE_A34 is worth asking for */
static bool contended;
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static void (*image_target)(unsigned target, void *image);     /* glEGLImageTargetTexture2DOES */
static char library[160] = "Mesa (EGL surfaceless, OpenGL ES): not started";

static const char *
egl_error(EGLint error)
{
	static char text[32];

	switch (error) {
	case EGL_SUCCESS: return "EGL_SUCCESS";
	case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
	case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
	case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
	case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
	case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
	case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
	case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
	case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
	case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
	case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
	default:
		snprintf(text, sizeof(text), "EGL error %#x", error);
		return text;
	}
}

static bool
has_extension(const char *list, const char *name)
{
	size_t length = strlen(name);

	for (const char *at = list; at != NULL && (at = strstr(at, name)) != NULL; at += length) {
		if ((at == list || at[-1] == ' ') && (at[length] == ' ' || at[length] == 0)) {
			return true;
		}
	}
	return false;
}

static void
display_start(void)
{
	EGLint major = 0, minor = 0, count = 0;
	const char *extensions;

	display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
		a34gl_say("a34gl: EGL (surfaceless) does not start: %s -- is the RedwoodMali service there?\n",
		    egl_error(eglGetError()));
		display = EGL_NO_DISPLAY;
		snprintf(library, sizeof(library), "Mesa (EGL surfaceless): no display");
		return;
	}
	extensions = eglQueryString(display, EGL_EXTENSIONS);
	for (int es = 3; es >= 2; es--) {
		const EGLint attributes[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, es == 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_NONE,
		};

		if (eglChooseConfig(display, attributes, &config, 1, &count) && count > 0) {
			config_apis = es;
			break;
		}
	}
	if (count < 1) {
		a34gl_say("a34gl: EGL %d.%d has no RGBA8888 pbuffer configuration for OpenGL ES\n", major, minor);
		eglTerminate(display);
		display = EGL_NO_DISPLAY;
		return;
	}
	create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	image_target = (void (*)(unsigned, void *))eglGetProcAddress("glEGLImageTargetTexture2DOES");
	images = create_image != NULL && destroy_image != NULL && image_target != NULL &&
	    has_extension(extensions, "EGL_KHR_image_base") && has_extension(extensions, "EGL_EXT_image_dma_buf_import");
	snprintf(library, sizeof(library), "Mesa %s (EGL %d.%d surfaceless, OpenGL ES %d)",
	    eglQueryString(display, EGL_VERSION), major, minor, config_apis);
	a34gl_say("a34gl: %s; vendor %s; IOSurfaces as EGLImages: %s\n", library, eglQueryString(display, EGL_VENDOR),
	    images ? "yes (EGL_IOSURFACE_A34)" :
	    "NO (no EGL_EXT_image_dma_buf_import: redwood-drm's DRM_CAP_PRIME, patches/0003-*): every surface is copied");
}

struct a34gl *
a34gl_create(int version)
{
	struct a34gl *gl;
	const EGLint window[] = { EGL_WIDTH, WINDOW, EGL_HEIGHT, WINDOW, EGL_NONE };

	(void)version;
	pthread_once(&display_once, display_start);
	if (display == EGL_NO_DISPLAY || (gl = calloc(1, sizeof(*gl))) == NULL) {
		return NULL;
	}
	eglBindAPI(EGL_OPENGL_ES_API);          /* (the calling thread's API) */
	for (gl->version = config_apis; gl->version >= 2; gl->version--) {
		const EGLint attributes[] = { EGL_CONTEXT_MAJOR_VERSION, gl->version, EGL_NONE };

		if ((gl->context = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes)) != EGL_NO_CONTEXT) {
			break;
		}
		a34gl_say("a34gl: no OpenGL ES %d context: %s\n", gl->version, egl_error(eglGetError()));
	}
	if (gl->context == EGL_NO_CONTEXT) {
		free(gl);
		return NULL;
	}
	if (gl->version < 3) {
		a34gl_say("a34gl: an OpenGL ES 2 context only: the programs of eagl.c's own (GLSL ES 3.00) will not be made\n");
	}
	if ((gl->surface = eglCreatePbufferSurface(display, config, window)) == EGL_NO_SURFACE) {
		a34gl_say("a34gl: no pbuffer to be current with: %s\n", egl_error(eglGetError()));
		eglDestroyContext(display, gl->context);
		free(gl);
		return NULL;
	}
	return gl;
}

int
a34gl_make_current(struct a34gl *gl)
{
	EGLint error;

	if (display == EGL_NO_DISPLAY) {
		return 0;
	}
	if (gl == NULL) {
		return eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}
	if (eglMakeCurrent(display, gl->surface, gl->surface, gl->context)) {
		return 1;
	}
	error = eglGetError();
	if (error == EGL_BAD_ACCESS) {
		contended = true;               /* current on another thread: see eagl.c, eagl_enter() */
	}
	a34gl_say("a34gl: eglMakeCurrent: %s%s\n", egl_error(error),
	    error == EGL_BAD_ACCESS ? " (the context is another thread's)" : "");
	return 0;
}

int
a34gl_contended(void)
{
	return contended;
}

void
a34gl_destroy(struct a34gl *gl)
{
	if (gl == NULL) {
		return;
	}
	eglDestroyContext(display, gl->context);
	eglDestroySurface(display, gl->surface);
	free(gl);
}

const char *
a34gl_library(void)
{
	return library;
}

int
a34gl_surface_texture(struct a34gl *gl, uint32_t surface)
{
	static unsigned refused, shared;
	EGLImageKHR image;
	unsigned error;

	(void)gl;
	if (!images || surface == 0) {
		return 0;
	}
	image = create_image(display, EGL_NO_CONTEXT, EGL_IOSURFACE_A34, (EGLClientBuffer)(uintptr_t)surface, NULL);
	if (image == EGL_NO_IMAGE_KHR) {
		EGLint why = eglGetError();

		if (refused++ < 24) {
			a34gl_say("a34gl: IOSurface %u is not the GPU's (%s): %s\n", surface, egl_error(why),
			    why == EGL_BAD_MATCH ? "a kind of surface not imported" : "the kernel does not describe it (0x7e)");
		}
		return 0;
	}
	while (glGetError() != GL_NO_ERROR) {
	}
	image_target(GL_TEXTURE_2D, image);
	error = glGetError();
	destroy_image(display, image);          /* (the texture holds the memory) */
	if (error != GL_NO_ERROR) {
		if (refused++ < 24) {
			a34gl_say("a34gl: IOSurface %u: glEGLImageTargetTexture2DOES: error %#x\n", surface, error);
		}
		return 0;
	}
	if (shared++ < 8) {
		a34gl_say("a34gl: IOSurface %u is a texture's memory\n", surface);
	}
	return 1;
}
