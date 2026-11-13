/*
 * The log of liba34gl and its loader: the file the environment variable A34GL_LOG names
 * (appended to; e.g. /var/mobile/a34gl.log), else stderr -- which, injected by the kernel into
 * backboardd, is the console: the kernel's log. One write() a line, so that the lines of two
 * libraries or two processes do not mix.
 *
 * a34gl_say() is rate-limited per message (per format string): the first SAY_FIRST times
 * each is written; after that at most one line every SAY_PERIOD_S seconds, which says how
 * many times the message has come so far. A message that repeats at every GL call can no
 * longer flood the kernel's log. a34gl_say_always() is for what a switch asked for (gltrace,
 * glframes, ...) and for the periodic summaries.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "a34gl.h"

#define SAY_FIRST       32
#define SAY_PERIOD_S    10
#define SAY_FORMATS     512             /* messages kept count of (more share one count) */

static pthread_once_t log_once = PTHREAD_ONCE_INIT;
static int log_fd = 2;

static void
open_log(void)
{
	const char *path = getenv("A34GL_LOG");
	int fd;

	if (path != NULL && *path != 0 && (fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)) >= 0) {
		log_fd = fd;
	}
}

int
a34gl_log_fd(void)
{
	pthread_once(&log_once, open_log);
	return log_fd;
}

static void
write_line(const char *format, va_list ap, const char *note)
{
	char line[1024];
	int n = vsnprintf(line, sizeof(line), format, ap);

	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (note != NULL) {
		/* the note goes at the end of the (first) line */
		size_t at = (size_t)n > 0 && line[n - 1] == '\n' ? (size_t)n - 1 : (size_t)n;
		int added = snprintf(line + at, sizeof(line) - at, "%s\n", note);

		n = added < 0 ? (int)at : (int)(at + (size_t)added);
		if (n > (int)sizeof(line) - 1) {
			n = sizeof(line) - 1;
		}
	}
	(void)write(a34gl_log_fd(), line, (size_t)n);
}

void
a34gl_say_always(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	write_line(format, ap, NULL);
	va_end(ap);
}

static struct said {
	const char *format;
	uint64_t count;
	time_t last;                    /* when it was last written (after the first SAY_FIRST) */
} said[SAY_FORMATS];
static pthread_mutex_t said_lock = PTHREAD_MUTEX_INITIALIZER;

void
a34gl_say(const char *format, ...)
{
	struct timespec now;
	uint64_t count = 0;
	bool write_it = false;
	char note[96] = "";
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &now);
	pthread_mutex_lock(&said_lock);
	for (size_t i = ((uintptr_t)format >> 3) % SAY_FORMATS, tries = 0; tries < SAY_FORMATS;
	    i = (i + 1) % SAY_FORMATS, tries++) {
		struct said *s = &said[i];

		if (s->format != format && s->format != NULL && tries + 1 < SAY_FORMATS) {
			continue;
		}
		s->format = s->format != NULL ? s->format : format;    /* (the table full: the last slot is shared) */
		count = ++s->count;
		if (count <= SAY_FIRST) {
			write_it = true;
			s->last = now.tv_sec;
		} else if (now.tv_sec - s->last >= SAY_PERIOD_S) {
			write_it = true;
			s->last = now.tv_sec;
		}
		break;
	}
	pthread_mutex_unlock(&said_lock);
	if (!write_it) {
		return;
	}
	if (count == SAY_FIRST) {
		snprintf(note, sizeof(note), " [%d times: from now on at most once in %d s]", SAY_FIRST, SAY_PERIOD_S);
	} else if (count > SAY_FIRST) {
		snprintf(note, sizeof(note), " [%llu times so far]", (unsigned long long)count);
	}
	va_start(ap, format);
	write_line(format, ap, note[0] != 0 ? note : NULL);
	va_end(ap);
}
