/*
 * a34bt-btd: bluetoothd's stand-in in its launchd job (tools/root-add-job.py --wrap /mnt com.apple.bluetoothd
 * /a34/a34bt-btd), as a34gl-bbd is backboardd's: it prepares the environment and execs the real bluetoothd
 * (A34GL_REAL, else /usr/sbin/bluetoothd) -- same pid, same Mach services.
 *
 *   - stdout and stderr on the console (the kernel's log): bluetoothd writes its start-up steps and failures with
 *     fprintf(stderr) as well as os_log ("Bluetooth Super Server Robot Destroyer", "Unable to attach local
 *     bluetooth session: %d", ...), and its job has no StandardErrorPath
 *   - OS_ACTIVITY_DT_MODE=1 only with the file /a34/bluetoothd.verbose (0975: by default the library writes the
 *     os_log lines that matter itself, quietly; see btdlib.c, 5).
 *   - DYLD_INSERT_LIBRARIES gets /a34/liba34bt-btd.dylib (btdlib.c): BlueTool answered inside bluetoothd, and
 *     every exit said with where it came from. The file /a34/bluetoothd.nolib leaves it out.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LIB "/a34/liba34bt-btd.dylib"

int
main(int argc, char **argv)
{
	const char *real = getenv("A34GL_REAL"), *old;
	char *both;
	int fd;

	(void)argc;
	if (real == NULL || *real == 0) {
		real = "/usr/sbin/bluetoothd";
	}
	if ((fd = open("/dev/console", O_WRONLY | O_NOCTTY)) >= 0) {
		(void)dup2(fd, 1);
		(void)dup2(fd, 2);
		if (fd > 2) {
			close(fd);
		}
	}
	if (access("/a34/bluetoothd.verbose", F_OK) == 0 && access("/a34/bluetoothd.quiet", F_OK) != 0) {
		setenv("OS_ACTIVITY_DT_MODE", "1", 1);
	}
	if (access("/a34/bluetoothd.nolib", F_OK) != 0) {
		old = getenv("DYLD_INSERT_LIBRARIES");
		if (old != NULL && *old != 0 && strstr(old, LIB) == NULL && asprintf(&both, "%s:%s", LIB, old) > 0) {
			setenv("DYLD_INSERT_LIBRARIES", both, 1);
		} else if (old == NULL || *old == 0) {
			setenv("DYLD_INSERT_LIBRARIES", LIB, 1);
		}
	}
	unsetenv("A34GL_REAL");
	fprintf(stderr, "a34bt-btd[%d]: exec %s (os_log on stderr: %s, %s)\n", (int)getpid(), real,
	    getenv("OS_ACTIVITY_DT_MODE") != NULL ? "yes" : "no", getenv("DYLD_INSERT_LIBRARIES") != NULL ? LIB : "no library");
	argv[0] = (char *)real;
	execv(real, argv);
	fprintf(stderr, "a34bt-btd: exec %s: %s\n", real, strerror(errno));
	return 127;
}
