/*
 * mali-tri: draw one triangle with Mesa's panfrost through EGL (surfaceless platform,
 * a 256x256 pbuffer) and an OpenGL ES context, read the pixels back and check them.
 *
 *   mali-tri [-2] [out.ppm]
 *
 * -2 asks for an OpenGL ES 2.0 context (default: 3.0, falling back to 2.0). With a file
 * name the frame is also written there as a binary PPM. Exit status: 0 the pixels are
 * right, 1 they are not, 2 EGL or GL could not be set up.
 *
 * Useful environment: EGL_LOG_LEVEL=debug, MESA_DEBUG=1, PAN_MESA_DEBUG=sync,
 * REDWOOD_DRM_DEBUG=1 (every request to the kernel driver).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EGL_NO_X11 1
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define SIZE 256

/* clear colour and triangle colour, as 8-bit RGBA */
static const uint8_t clear_rgba[4] = { 0x00, 0x40, 0x80, 0xff };
static const uint8_t tri_rgba[4] = { 0xff, 0x80, 0x00, 0xff };

static const char vs_source[] =
	"attribute vec2 position;\n"
	"void main() {\n"
	"    gl_Position = vec4(position, 0.0, 1.0);\n"
	"}\n";

static const char fs_source[] =
	"precision mediump float;\n"
	"uniform vec4 color;\n"
	"void main() {\n"
	"    gl_FragColor = color;\n"
	"}\n";

static double
seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static const char *
egl_error(void)
{
	static char text[32];
	EGLint e = eglGetError();

	switch (e) {
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
		snprintf(text, sizeof(text), "EGL error %#x", e);
		return text;
	}
}

static GLuint
shader(GLenum type, const char *source)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	char log[1024];

	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "mali-tri: %s shader: %s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		return 0;
	}
	return s;
}

static int
near(const uint8_t *pixel, const uint8_t *want)
{
	for (int i = 0; i < 4; i++) {
		int d = (int)pixel[i] - (int)want[i];

		if (d < -2 || d > 2) {
			return 0;
		}
	}
	return 1;
}

static void
show(const char *name, const uint8_t *frame, int x, int y)
{
	const uint8_t *p = frame + 4 * (y * SIZE + x);

	printf("pixel %-12s (%3d,%3d) = %02x %02x %02x %02x\n", name, x, y, p[0], p[1], p[2], p[3]);
}

static void
write_ppm(const char *path, const uint8_t *frame)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL) {
		perror(path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", SIZE, SIZE);
	for (int y = SIZE - 1; y >= 0; y--) {          /* GL's rows go up, PPM's down */
		for (int x = 0; x < SIZE; x++) {
			fwrite(frame + 4 * (y * SIZE + x), 1, 3, f);
		}
	}
	fclose(f);
	printf("wrote %s\n", path);
}

int
main(int argc, char **argv)
{
	const char *ppm = NULL;
	int es_major = 3;
	double t0 = seconds();

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-2") == 0) {
			es_major = 2;
		} else {
			ppm = argv[i];
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
	EGLint major = 0, minor = 0;

	if (dpy == EGL_NO_DISPLAY) {
		fprintf(stderr, "mali-tri: eglGetPlatformDisplay(surfaceless): %s\n", egl_error());
		return 2;
	}
	if (!eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "mali-tri: eglInitialize: %s (is the RedwoodMali service there?)\n", egl_error());
		return 2;
	}
	printf("EGL %d.%d  vendor: %s  version: %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR),
	    eglQueryString(dpy, EGL_VERSION));
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "mali-tri: eglBindAPI: %s\n", egl_error());
		return 2;
	}

	const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	EGLConfig config;
	EGLint configs = 0;

	if (!eglChooseConfig(dpy, config_attribs, &config, 1, &configs) || configs < 1) {
		fprintf(stderr, "mali-tri: no RGBA8888 pbuffer config: %s\n", egl_error());
		return 2;
	}

	const EGLint surface_attribs[] = { EGL_WIDTH, SIZE, EGL_HEIGHT, SIZE, EGL_NONE };
	EGLSurface surface = eglCreatePbufferSurface(dpy, config, surface_attribs);

	if (surface == EGL_NO_SURFACE) {
		fprintf(stderr, "mali-tri: eglCreatePbufferSurface: %s\n", egl_error());
		return 2;
	}

	EGLContext context = EGL_NO_CONTEXT;

	for (; es_major >= 2 && context == EGL_NO_CONTEXT; es_major--) {
		const EGLint context_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, es_major, EGL_NONE };

		context = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attribs);
		if (context == EGL_NO_CONTEXT) {
			fprintf(stderr, "mali-tri: no OpenGL ES %d context: %s\n", es_major, egl_error());
		}
	}
	if (context == EGL_NO_CONTEXT) {
		return 2;
	}
	if (!eglMakeCurrent(dpy, surface, surface, context)) {
		fprintf(stderr, "mali-tri: eglMakeCurrent: %s\n", egl_error());
		return 2;
	}
	printf("GL_VENDOR:   %s\n", (const char *)glGetString(GL_VENDOR));
	printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
	printf("GL_VERSION:  %s\n", (const char *)glGetString(GL_VERSION));
	printf("GLSL:        %s\n", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
	printf("context ready after %.3f s\n", seconds() - t0);

	GLuint vs = shader(GL_VERTEX_SHADER, vs_source);
	GLuint fs = shader(GL_FRAGMENT_SHADER, fs_source);
	GLuint program = glCreateProgram();
	GLint linked = 0;

	if (vs == 0 || fs == 0) {
		return 2;
	}
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glBindAttribLocation(program, 0, "position");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[1024];

		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		fprintf(stderr, "mali-tri: link: %s\n", log);
		return 2;
	}

	/* a triangle over the middle; the corners stay the clear colour */
	static const GLfloat vertices[] = {
		-0.5f, -0.5f,
		 0.5f, -0.5f,
		 0.0f,  0.5f,
	};
	GLuint vbo;

	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	glViewport(0, 0, SIZE, SIZE);
	glClearColor(clear_rgba[0] / 255.0f, clear_rgba[1] / 255.0f, clear_rgba[2] / 255.0f, clear_rgba[3] / 255.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(program);
	glUniform4f(glGetUniformLocation(program, "color"), tri_rgba[0] / 255.0f, tri_rgba[1] / 255.0f,
	    tri_rgba[2] / 255.0f, tri_rgba[3] / 255.0f);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	uint8_t *frame = calloc(SIZE * SIZE, 4);
	double t1 = seconds();

	if (frame == NULL) {
		return 2;
	}
	glFinish();
	glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, frame);
	GLenum error = glGetError();

	printf("draw + readback %.3f s, glGetError() = %#x\n", seconds() - t1, error);

	show("corner", frame, 2, 2);
	show("corner", frame, SIZE - 3, SIZE - 3);
	show("centre", frame, SIZE / 2, SIZE / 2);
	show("near apex", frame, SIZE / 2, SIZE * 5 / 8);
	show("left base", frame, SIZE * 5 / 16, SIZE * 5 / 16);

	int ok = error == GL_NO_ERROR &&
	    near(frame + 4 * (2 * SIZE + 2), clear_rgba) &&
	    near(frame + 4 * ((SIZE - 3) * SIZE + SIZE - 3), clear_rgba) &&
	    near(frame + 4 * ((SIZE / 2) * SIZE + SIZE / 2), tri_rgba) &&
	    near(frame + 4 * ((SIZE * 5 / 8) * SIZE + SIZE / 2), tri_rgba);
	unsigned covered = 0;

	for (int i = 0; i < SIZE * SIZE; i++) {
		covered += near(frame + 4 * i, tri_rgba);
	}
	/* the triangle covers an eighth of the viewport: 8192 of 65536 pixels */
	printf("triangle pixels: %u (expected about %u)\n", covered, SIZE * SIZE / 8);
	ok = ok && covered > SIZE * SIZE / 8 - SIZE && covered < SIZE * SIZE / 8 + SIZE;

	if (ppm != NULL) {
		write_ppm(ppm, frame);
	}
	free(frame);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, context);
	eglDestroySurface(dpy, surface);
	eglTerminate(dpy);

	printf("%s (%.3f s)\n", ok ? "PASS" : "FAIL", seconds() - t0);
	return ok ? 0 : 1;
}
