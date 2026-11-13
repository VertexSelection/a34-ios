/*
 * The log of liba34gl and its loader: the file the environment variable A34GL_LOG names
 * (appended to; e.g. /var/mobile/a34gl.log, or /dev/console where the job's user may write
 * it), else stderr -- which launchd sends to /dev/null for backboardd. One write() a line, so
 * that the lines of two libraries or two processes do not mix.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "a34gl.h"

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

void
a34gl_say(const char *format, ...)
{
	char line[1024];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	(void)write(a34gl_log_fd(), line, (size_t)n);
}
