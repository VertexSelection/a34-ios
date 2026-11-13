/*
 * a34gl-surftest: the IOSurface path of liba34gl, without CoreAnimation. Run it before putting
 * liba34gl into backboardd.
 *
 *   a34gl-surftest [-v] [WIDTH HEIGHT]        (default 320 x 200)
 *
 * Makes two 'BGRA' IOSurfaces (IOSurface.framework, rows of a multiple of 64 bytes), and with
 * Mesa's panfrost through EGL (surfaceless) and OpenGL ES 3:
 *
 *   describe  redwood_drm_surface(): what the kernel says of the surface (RedwoodMali 0x7e,
 *             asked only to describe)
 *   import    eglCreateImageKHR(EGL_IOSURFACE_A34, the surface's ID) and
 *             glEGLImageTargetTexture2DOES: the surface's memory as a texture (0x7e again, for a
 *             GEM handle, and PANFROST_GET_BO_OFFSET for its GPU address)
 *   1 draw    into surface A through a framebuffer: a clear, a scissored clear, a triangle;
 *             checked with glReadPixels and in the surface's memory as the CPU sees it
 *             (IOSurfaceLock): the GPU's writes reach the surface
 *   2 sample  surface B, filled by the CPU (four quadrants), drawn into a texture of the GPU's
 *             own and read back: the CPU's writes reach the GPU
 *   3 blit    that texture into surface A with glBlitFramebuffer (eagl.c copies a frame into
 *             the display's surface so), checked in A's memory
 *   4 threads the context moves between threads as CoreAnimation moves it (Mesa patch 0004):
 *             a second thread makes it current while this one has it and clears part of
 *             surface A; this thread takes it back and checks; the second thread, whose
 *             binding is stale then, lets go (eglMakeCurrent(EGL_NO_CONTEXT)); this thread
 *             draws again and checks
 *
 * Prints what it finds and PASS or FAIL. Exit status: 0 everything right; 1 wrong pixels;
 * 2 EGL / OpenGL ES not to be had; 3 an IOSurface could not be imported (the kernel's 0x7e,
 * or a surface the GPU cannot use). -v: REDWOOD_DRM_DEBUG=1 (every request to the kernel)
 * and EGL_LOG_LEVEL=debug.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define EGL_NO_X11 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglmesaext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <redwood_drm.h>

#define BGRA    0x42475241u

struct colour {
	uint8_t r, g, b, a;
};

static const struct colour clear_colour = { 0x20, 0x40, 0x80, 0xff };
static const struct colour box_colour = { 0xff, 0xff, 0x00, 0xff };
static const struct colour tri_colour = { 0xff, 0x80, 0x00, 0xff };
static const struct colour quadrants[4] = {     /* surface B: bottom left, bottom right, top left, top right (GL's y up) */
	{ 0xff, 0x00, 0x00, 0xff }, { 0x00, 0xff, 0x00, 0xff }, { 0x00, 0x00, 0xff, 0xff }, { 0xff, 0xff, 0xff, 0x80 },
};

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
static EGLDisplay display;
static int width = 320, height = 200;
static unsigned failures;

static double
seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void
put(CFMutableDictionaryRef d, CFStringRef key, long value)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongType, &value);

	CFDictionarySetValue(d, key, n);
	CFRelease(n);
}

static IOSurfaceRef
make_surface(void)
{
	CFMutableDictionaryRef p = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	long row = ((long)width * 4 + 63) & ~63L;       /* panfrost (Valhall) wants linear rows of 64 bytes */
	IOSurfaceRef s;

	put(p, kIOSurfaceWidth, width);
	put(p, kIOSurfaceHeight, height);
	put(p, kIOSurfaceBytesPerElement, 4);
	put(p, kIOSurfaceBytesPerRow, row);
	put(p, kIOSurfaceAllocSize, row * height);
	put(p, kIOSurfacePixelFormat, (long)BGRA);
	s = IOSurfaceCreate(p);
	CFRelease(p);
	if (s == NULL) {
		printf("IOSurfaceCreate(%d x %d, 'BGRA', %ld bytes a row) failed\n", width, height, row);
		return NULL;
	}
	printf("IOSurface %u: %zu x %zu 'BGRA', %zu bytes a row, %zu bytes, %zu planes\n", IOSurfaceGetID(s),
	    IOSurfaceGetWidth(s), IOSurfaceGetHeight(s), IOSurfaceGetBytesPerRow(s), IOSurfaceGetAllocSize(s),
	    IOSurfaceGetPlaneCount(s));
	return s;
}

static void
describe(IOSurfaceRef s)
{
	struct redwood_drm_surface d = { .surface = IOSurfaceGetID(s) };

	if (redwood_drm_surface(-1, &d) != 0) {
		printf("describe: the kernel does not describe IOSurface %u: %s (selector 0x7e)\n", d.surface, strerror(errno));
		return;
	}
	printf("describe: IOSurface %u: buffer %llu bytes, pixels from %u, %u bytes a row, %u x %u, format %#x, %u planes\n",
	    d.surface, (unsigned long long)d.size, d.offset, d.row_bytes, d.width, d.height, d.pixel_format, d.planes);
	if (d.width != IOSurfaceGetWidth(s) || d.height != IOSurfaceGetHeight(s) || d.row_bytes != IOSurfaceGetBytesPerRow(s) ||
	    d.pixel_format != BGRA || d.planes != 0) {
		printf("describe: DIFFERENT from what IOSurface.framework says\n");
		failures++;
	}
}

/* A texture made of the surface's memory, bound to GL_TEXTURE_2D; 0: none. */
static GLuint
import(IOSurfaceRef s)
{
	EGLImageKHR image;
	GLuint texture;
	GLenum error;
	double t0 = seconds();

	image = create_image(display, EGL_NO_CONTEXT, EGL_IOSURFACE_A34, (EGLClientBuffer)(uintptr_t)IOSurfaceGetID(s), NULL);
	if (image == EGL_NO_IMAGE_KHR) {
		printf("import: eglCreateImageKHR(EGL_IOSURFACE_A34, %u): EGL error %#x\n", IOSurfaceGetID(s), eglGetError());
		return 0;
	}
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	image_target(GL_TEXTURE_2D, image);
	error = glGetError();
	destroy_image(display, image);
	if (error != GL_NO_ERROR) {
		printf("import: glEGLImageTargetTexture2DOES: error %#x\n", error);
		glDeleteTextures(1, &texture);
		return 0;
	}
	printf("import: IOSurface %u is texture %u (%.3f ms)\n", IOSurfaceGetID(s), texture, (seconds() - t0) * 1e3);
	return texture;
}

static GLuint
framebuffer_of(GLuint texture)
{
	GLuint fb;
	GLenum status;

	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		printf("framebuffer of texture %u: incomplete (%#x)\n", texture, status);
		return 0;
	}
	return fb;
}

static GLuint
program(const char *vertex, const char *fragment)
{
	const char *sources[2] = { vertex, fragment };
	GLenum kinds[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	GLint ok = 0;
	char log[512];

	for (int i = 0; i < 2; i++) {
		GLuint sh = glCreateShader(kinds[i]);

		glShaderSource(sh, 1, &sources[i], NULL);
		glCompileShader(sh);
		glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(sh, sizeof(log), NULL, log);
			printf("shader: %s\n", log);
			return 0;
		}
		glAttachShader(p, sh);
	}
	glBindAttribLocation(p, 0, "position");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		printf("program: %s\n", log);
		return 0;
	}
	return p;
}

static bool
near(struct colour got, struct colour want)
{
	int d[4] = { got.r - want.r, got.g - want.g, got.b - want.b, got.a - want.a };

	for (int i = 0; i < 4; i++) {
		if (d[i] < -2 || d[i] > 2) {
			return false;
		}
	}
	return true;
}

/* One pixel, read with glReadPixels from the framebuffer bound. */
static struct colour
gl_pixel(int x, int y)
{
	uint8_t p[4] = { 0, 0, 0, 0 };

	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	return (struct colour){ p[0], p[1], p[2], p[3] };
}

/* One pixel, as the CPU sees it in the surface's memory (row y of memory is GL's y). */
static struct colour
memory_pixel(IOSurfaceRef s, int x, int y)
{
	const uint8_t *p = (const uint8_t *)IOSurfaceGetBaseAddress(s) + (size_t)y * IOSurfaceGetBytesPerRow(s) + (size_t)x * 4;

	return (struct colour){ p[2], p[1], p[0], p[3] };       /* B, G, R, A in memory */
}

static void
check(const char *what, const char *where, int x, int y, struct colour got, struct colour want)
{
	bool ok = near(got, want);

	printf("  %-6s %-8s (%3d,%3d) = %02x %02x %02x %02x, want %02x %02x %02x %02x%s\n", what, where, x, y, got.r, got.g,
	    got.b, got.a, want.r, want.g, want.b, want.a, ok ? "" : "  WRONG");
	failures += !ok;
}

static const char vertex_source[] =
    "#version 300 es\n"
    "in vec2 position;\n"
    "out vec2 at;\n"
    "void main() { at = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }\n";
static const char colour_source[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 colour;\n"
    "out vec4 result;\n"
    "void main() { result = colour; }\n";
static const char texture_source[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D picture;\n"
    "in vec2 at;\n"
    "out vec4 result;\n"
    "void main() { result = texture(picture, at); }\n";

/* test 4: the second thread, two steps, one at a time with the main thread */
static struct {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	int step;                       /* 1: worker takes over; 2: main has taken back; 3: worker let go */
	EGLSurface surface;
	EGLContext context;
	GLuint framebuffer;
	EGLBoolean took, let_go;
	EGLint error;
} relay = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, 0, 0 };

static const struct colour thread_colour = { 0x10, 0xe0, 0x30, 0xff };

static void
relay_wait(int step)
{
	pthread_mutex_lock(&relay.lock);
	while (relay.step < step) {
		pthread_cond_wait(&relay.changed, &relay.lock);
	}
	pthread_mutex_unlock(&relay.lock);
}

static void
relay_go(int step)
{
	pthread_mutex_lock(&relay.lock);
	relay.step = step;
	pthread_cond_broadcast(&relay.changed);
	pthread_mutex_unlock(&relay.lock);
}

static void *
worker(void *unused)
{
	(void)unused;
	eglBindAPI(EGL_OPENGL_ES_API);
	relay.took = eglMakeCurrent(display, relay.surface, relay.surface, relay.context);   /* current on main: taken over */
	relay.error = eglGetError();
	if (relay.took) {
		glBindFramebuffer(GL_FRAMEBUFFER, relay.framebuffer);
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, 0, width / 2, height / 2);
		glClearColor(thread_colour.r / 255.0f, thread_colour.g / 255.0f, thread_colour.b / 255.0f,
		    thread_colour.a / 255.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		glFinish();
	}
	relay_go(1);
	relay_wait(2);
	relay.let_go = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);  /* stale: only lets go */
	relay_go(3);
	return NULL;
}

static void
quad(void)
{
	static const GLfloat corners[] = { -1, -1, 1, -1, -1, 1, 1, 1 };

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

int
main(int argc, char **argv)
{
	EGLint major = 0, minor = 0, count = 0;
	EGLConfig config;
	EGLContext context = EGL_NO_CONTEXT;
	EGLSurface pbuffer = EGL_NO_SURFACE;
	IOSurfaceRef a, b;
	GLuint ta, tb, fa, own, fown, colour_program, texture_program;
	int bx0, by0, bx1, by1;
	double t0 = seconds();
	int arg = 1;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (arg < argc && strcmp(argv[arg], "-v") == 0) {
		setenv("REDWOOD_DRM_DEBUG", "1", 1);
		setenv("EGL_LOG_LEVEL", "debug", 1);
		arg++;
	}
	if (arg + 1 < argc) {
		width = atoi(argv[arg]);
		height = atoi(argv[arg + 1]);
	}
	if (width < 16 || height < 16 || width > 8192 || height > 8192) {
		fprintf(stderr, "usage: a34gl-surftest [-v] [WIDTH HEIGHT]\n");
		return 2;
	}

	/* EGL and OpenGL ES, as liba34gl has them (gl_egl.c) */
	display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
		printf("EGL: no display (error %#x): is the RedwoodMali service there?\nFAIL\n", eglGetError());
		return 2;
	}
	const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
	printf("EGL %d.%d %s; EXT_image_dma_buf_import: %s\n", major, minor, eglQueryString(display, EGL_VERSION),
	    extensions && strstr(extensions, "EGL_EXT_image_dma_buf_import") ? "yes" : "NO");
	const EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
	};
	const EGLint context_attributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
	const EGLint pbuffer_attributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	eglBindAPI(EGL_OPENGL_ES_API);
	if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count < 1 ||
	    (context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes)) == EGL_NO_CONTEXT ||
	    (pbuffer = eglCreatePbufferSurface(display, config, pbuffer_attributes)) == EGL_NO_SURFACE ||
	    !eglMakeCurrent(display, pbuffer, pbuffer, context)) {
		printf("EGL: no OpenGL ES 3 context (error %#x)\nFAIL\n", eglGetError());
		return 2;
	}
	printf("GL_RENDERER %s; GL_VERSION %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
	create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (create_image == NULL || destroy_image == NULL || image_target == NULL) {
		printf("EGL: no EGLImage functions\nFAIL\n");
		return 2;
	}
	colour_program = program(vertex_source, colour_source);
	texture_program = program(vertex_source, texture_source);
	if (colour_program == 0 || texture_program == 0) {
		printf("FAIL\n");
		return 2;
	}

	/* the surfaces, and what the kernel says of them */
	if ((a = make_surface()) == NULL || (b = make_surface()) == NULL) {
		printf("FAIL\n");
		return 3;
	}
	describe(a);
	if ((ta = import(a)) == 0 || (fa = framebuffer_of(ta)) == 0) {
		printf("FAIL (the IOSurface is not the GPU's)\n");
		return 3;
	}

	/* 1: the GPU draws into surface A */
	printf("1 draw into IOSurface %u:\n", IOSurfaceGetID(a));
	bx0 = width / 8, by0 = height / 8, bx1 = width / 4, by1 = height / 4;
	glBindFramebuffer(GL_FRAMEBUFFER, fa);
	glViewport(0, 0, width, height);
	glClearColor(clear_colour.r / 255.0f, clear_colour.g / 255.0f, clear_colour.b / 255.0f, clear_colour.a / 255.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	glScissor(bx0, by0, bx1 - bx0, by1 - by0);
	glClearColor(box_colour.r / 255.0f, box_colour.g / 255.0f, box_colour.b / 255.0f, box_colour.a / 255.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	{
		static const GLfloat triangle[] = { 0.0f, -0.5f, 0.9f, -0.5f, 0.45f, 0.9f };    /* the right half */

		glUseProgram(colour_program);
		glUniform4f(glGetUniformLocation(colour_program, "colour"), tri_colour.r / 255.0f, tri_colour.g / 255.0f,
		    tri_colour.b / 255.0f, tri_colour.a / 255.0f);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, triangle);
		glEnableVertexAttribArray(0);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	glFinish();
	printf("  drawn, glGetError %#x\n", glGetError());
	{
		const struct { const char *what; int x, y; struct colour want; } points[] = {
			{ "corner", 2, 2, clear_colour },
			{ "corner", width - 3, height - 3, clear_colour },
			{ "box", (bx0 + bx1) / 2, (by0 + by1) / 2, box_colour },
			{ "tri", width * 29 / 40, height * 2 / 5, tri_colour },
			{ "left", width / 4, height * 3 / 4, clear_colour },
		};

		for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
			check(points[i].what, "GL", points[i].x, points[i].y, gl_pixel(points[i].x, points[i].y), points[i].want);
		}
		if (IOSurfaceLock(a, kIOSurfaceLockReadOnly, NULL) != 0) {
			printf("  IOSurfaceLock failed\n");
			failures++;
		} else {
			for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
				check(points[i].what, "memory", points[i].x, points[i].y, memory_pixel(a, points[i].x, points[i].y),
				    points[i].want);
			}
			IOSurfaceUnlock(a, kIOSurfaceLockReadOnly, NULL);
		}
	}

	/* 2: the CPU fills surface B, the GPU draws with it into a texture of its own */
	printf("2 sample IOSurface %u, filled by the CPU:\n", IOSurfaceGetID(b));
	describe(b);
	if (IOSurfaceLock(b, 0, NULL) != 0) {
		printf("  IOSurfaceLock failed\nFAIL\n");
		return 1;
	}
	for (int y = 0; y < height; y++) {
		uint8_t *row = (uint8_t *)IOSurfaceGetBaseAddress(b) + (size_t)y * IOSurfaceGetBytesPerRow(b);

		for (int x = 0; x < width; x++) {
			struct colour c = quadrants[(y >= height / 2) * 2 + (x >= width / 2)];

			row[4 * x] = c.b, row[4 * x + 1] = c.g, row[4 * x + 2] = c.r, row[4 * x + 3] = c.a;
		}
	}
	IOSurfaceUnlock(b, 0, NULL);
	if ((tb = import(b)) == 0) {
		printf("FAIL (the IOSurface is not the GPU's)\n");
		return 3;
	}
	glGenTextures(1, &own);
	glBindTexture(GL_TEXTURE_2D, own);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
	if ((fown = framebuffer_of(own)) == 0) {
		printf("FAIL\n");
		return 2;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, fown);
	glViewport(0, 0, width, height);
	glUseProgram(texture_program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tb);
	glUniform1i(glGetUniformLocation(texture_program, "picture"), 0);
	quad();
	glFinish();
	printf("  drawn, glGetError %#x\n", glGetError());
	for (int q = 0; q < 4; q++) {
		int x = (q & 1) ? width * 3 / 4 : width / 4, y = (q & 2) ? height * 3 / 4 : height / 4;

		check("quad", "GL", x, y, gl_pixel(x, y), quadrants[q]);
	}

	/* 3: that texture into surface A, by the GPU's blit */
	printf("3 blit into IOSurface %u:\n", IOSurfaceGetID(a));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fown);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fa);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glFinish();
	printf("  blitted, glGetError %#x\n", glGetError());
	if (IOSurfaceLock(a, kIOSurfaceLockReadOnly, NULL) == 0) {
		for (int q = 0; q < 4; q++) {
			int x = (q & 1) ? width * 3 / 4 : width / 4, y = (q & 2) ? height * 3 / 4 : height / 4;

			check("quad", "memory", x, y, memory_pixel(a, x, y), quadrants[q]);
		}
		IOSurfaceUnlock(a, kIOSurfaceLockReadOnly, NULL);
	} else {
		printf("  IOSurfaceLock failed\n");
		failures++;
	}

	/* 4: the context moves to another thread and back (EAGL's semantics, Mesa patch 0004) */
	printf("4 threads:\n");
	{
		pthread_t thread;

		relay.surface = pbuffer;
		relay.context = context;
		relay.framebuffer = fa;
		if (pthread_create(&thread, NULL, worker, NULL) != 0) {
			printf("  pthread_create failed\n");
			failures++;
		} else {
			relay_wait(1);
			printf("  second thread: eglMakeCurrent of the context current here: %s (EGL error %#x)\n",
			    relay.took ? "taken over" : "REFUSED", relay.error);
			failures += !relay.took;
			if (!eglMakeCurrent(display, pbuffer, pbuffer, context)) {
				printf("  taking it back: EGL error %#x\n", eglGetError());
				failures++;
			}
			glBindFramebuffer(GL_FRAMEBUFFER, fa);
			check("thread", "GL", width / 4, height / 4, gl_pixel(width / 4, height / 4),
			    relay.took ? thread_colour : quadrants[0]);
			relay_go(2);
			relay_wait(3);
			printf("  second thread, its binding stale: eglMakeCurrent(EGL_NO_CONTEXT): %s\n",
			    relay.let_go ? "ok" : "FAILED");
			failures += !relay.let_go;
			pthread_join(thread, NULL);
			/* still this thread's, and whole */
			glBindFramebuffer(GL_FRAMEBUFFER, fa);
			glViewport(0, 0, width, height);
			glClearColor(box_colour.r / 255.0f, box_colour.g / 255.0f, box_colour.b / 255.0f, box_colour.a / 255.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			glFinish();
			check("after", "GL", width / 2, height / 2, gl_pixel(width / 2, height / 2), box_colour);
			if (IOSurfaceLock(a, kIOSurfaceLockReadOnly, NULL) == 0) {
				check("after", "memory", width / 2, height / 2, memory_pixel(a, width / 2, height / 2), box_colour);
				IOSurfaceUnlock(a, kIOSurfaceLockReadOnly, NULL);
			}
			printf("  glGetError %#x\n", glGetError());
		}
	}

	glDeleteFramebuffers(1, &fa);
	glDeleteFramebuffers(1, &fown);
	glDeleteTextures(1, &ta);
	glDeleteTextures(1, &tb);
	glDeleteTextures(1, &own);
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	eglDestroySurface(display, pbuffer);
	eglTerminate(display);
	CFRelease(a);
	CFRelease(b);
	printf("%s (%u wrong, %.3f s)\n", failures == 0 ? "PASS" : "FAIL", failures, seconds() - t0);
	return failures == 0 ? 0 : 1;
}
