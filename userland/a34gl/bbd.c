/*
 * a34gl-bbd: stands in for backboardd in its launchd job (tools/root-add-job.py --wrap).
 * launchd does not pass DYLD_* variables from a job description to the program, so this adds
 * our loader to DYLD_INSERT_LIBRARIES and execs the real window server: same pid, same Mach
 * services launchd prepared, the real binary's code signature and entitlements from the exec on.
 *
 *   the real program   $A34GL_REAL (root-add-job.py --wrap puts the job's own there),
 *                      else /usr/libexec/backboardd
 *   the loader         /a34/liba34gl-inject.dylib, when it is there; not with boot-arg a34gl=0
 *                      (nor the environment file), nor with A34GL_OFF=1
 *   more environment   /a34/backboardd.env, when it is there: KEY=VALUE lines, # comments
 *                      (A34GL=..., A34GL_LOG=..., PAN_MESA_DEBUG=..., REDWOOD_DRM_DEBUG=1, ...)
 *
 * dyld honours DYLD_INSERT_LIBRARIES only when the kernel's AMFI answers its policy question
 * (amfi_check_dyld_policy_self) with "allow": see README.md.
 *
 * Ported from ios-topaz's userland/gpu/bbd_wrap.c (MIT).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#ifndef A34GL_DIR
#define A34GL_DIR       "/a34"
#endif
#define REAL            "/usr/libexec/backboardd"
#define LOADER          A34GL_DIR "/liba34gl-inject.dylib"
#define ENVFILE         A34GL_DIR "/backboardd.env"

static void
environment_file(void)
{
	FILE *f = fopen(ENVFILE, "r");
	char line[512];

	if (f == NULL) {
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char *eq = strchr(line, '='), *nl = strchr(line, '\n');

		if (nl != NULL) {
			*nl = 0;
		}
		if (eq != NULL && line[0] != '#' && eq != line) {
			*eq = 0;
			setenv(line, eq + 1, 1);
		}
	}
	fclose(f);
}

int
main(int argc, char *argv[])
{
	const char *real = getenv("A34GL_REAL");
	char bootargs[1024] = "";
	size_t blen = sizeof(bootargs) - 1;

	(void)argc;
	if (real == NULL || *real == 0) {
		real = REAL;
	}
	real = strdup(real);
	unsetenv("A34GL_REAL");
	(void)sysctlbyname("kern.bootargs", bootargs, &blen, NULL, 0);
	if (strstr(bootargs, "a34gl=0") == NULL) {
		const char *off;

		environment_file();
		off = getenv("A34GL_OFF");
		if ((off == NULL || strcmp(off, "1") != 0) && access(LOADER, R_OK) == 0) {
			const char *had = getenv("DYLD_INSERT_LIBRARIES");
			char *both = NULL;

			if (had != NULL && *had != 0 && asprintf(&both, "%s:%s", LOADER, had) > 0) {
				setenv("DYLD_INSERT_LIBRARIES", both, 1);
				free(both);
			} else {
				setenv("DYLD_INSERT_LIBRARIES", LOADER, 1);
			}
		}
	}
	argv[0] = (char *)real;
	execv(real, argv);
	perror(real);
	return 127;
}
