/*
 * Where a frame's time goes, as kept lines every five seconds ("rwm perf", "rwm waits", "rwm making",
 * "rwm batches", "rwm draw us"): what the window server's frame rate is made of, measured rather than
 * guessed. The counters are added to where the work is (the plug-in and the core alike); the formats of
 * the lines are kept to the character. And the boot arguments, read once.
 */
#include "rwm.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

struct rwm_perf rwm_perf;

namespace {

void
say(const char *line)
{
	/* A34: through stdout (the devnull tee in backboardd), as "redwoodmetal camprof: ..." */
	printf("redwoodmetal camprof: %s\n", line);
	fflush(stdout);
}

/* one of the compiler library's profile calls, if it is loaded and has it (not part of the contract) */
void *
profile_call(const char *name)
{
	void *lib = dlopen("/a34/libgallium-26.3.0.dylib", RTLD_NOW | RTLD_NOLOAD);

	return lib != nullptr ? dlsym(lib, name) : nullptr;
}

} // namespace

extern "C" void *
rwm_profile_call(const char *name)
{
	return profile_call(name);
}

/* A34: the slowest command buffer of the period, in parts ("rwm worst cb") */
static struct rwm_cb_parts worst_cb;

extern "C" void
rwm_cb_parts_note(const struct rwm_cb_parts *parts)
{
	if (parts->took_ns > worst_cb.took_ns) {
		worst_cb = *parts;
	}
}

static void
say_worst_cb(void)
{
	const struct rwm_cb_parts &w = worst_cb;
	const uint64_t *l = w.lib;
	static const char *const kinds[] = { "?", "draw", "dispatch", "copy" };
	char line[900];

	if (w.took_ns == 0) {
		return;
	}
	snprintf(line, sizeof(line), "rwm worst cb %.1f ms: %u works; %llu draws %.1f ms, %llu copies %.1f ms, "
		 "%llu dispatches %.1f ms, %llu copied in %.1f ms, submit %.1f ms; slowest work #%u (a %s) %.1f ms; "
		 "library: %llu batches %.1f ms (%llu from inside a draw), %llu buffers made %.1f ms (%llu from the kernel "
		 "%.1f ms), %llu variants %.1f ms, %llu pipelines %.1f ms, %llu textures made %.1f ms (any thread), "
		 "%llu staging copies; kernel (whole process): CREATE_BO x%llu %.1f ms, maps x%llu %.1f ms, "
		 "SURFACE x%llu %.1f ms, SUBMIT x%llu %.1f ms, WAIT_BO x%llu %.1f ms, GEM_CLOSE x%llu %.1f ms",
		 w.took_ns / 1e6, w.works, (unsigned long long)w.draws, w.draw_ns / 1e6, (unsigned long long)w.blits,
		 w.blit_ns / 1e6, (unsigned long long)w.dispatches, w.dispatch_ns / 1e6, (unsigned long long)w.copyins,
		 w.copyin_ns / 1e6, w.submit_ns / 1e6, w.slowest_index, kinds[w.slowest_kind < 4 ? w.slowest_kind : 0],
		 w.slowest_ns / 1e6, (unsigned long long)l[RWA_CB_BATCHES], l[RWA_CB_BATCH_NS] / 1e6,
		 (unsigned long long)l[RWA_CB_DEP_FLUSHES], (unsigned long long)l[RWA_CB_BO_MADE],
		 l[RWA_CB_BO_MADE_NS] / 1e6, (unsigned long long)l[RWA_CB_BO_FRESH], l[RWA_CB_BO_FRESH_NS] / 1e6,
		 (unsigned long long)l[RWA_CB_VARIANTS], l[RWA_CB_VARIANT_NS] / 1e6, (unsigned long long)l[RWA_CB_PIPELINES],
		 l[RWA_CB_PIPELINE_NS] / 1e6, (unsigned long long)l[RWA_CB_TEXTURES], l[RWA_CB_TEXTURE_NS] / 1e6,
		 (unsigned long long)l[RWA_CB_STAGING], (unsigned long long)l[RWA_CB_K_CREATE], l[RWA_CB_K_CREATE_NS] / 1e6,
		 (unsigned long long)l[RWA_CB_K_MAPS], l[RWA_CB_K_MAPS_NS] / 1e6, (unsigned long long)l[RWA_CB_K_SURFACE],
		 l[RWA_CB_K_SURFACE_NS] / 1e6, (unsigned long long)l[RWA_CB_K_SUBMIT], l[RWA_CB_K_SUBMIT_NS] / 1e6,
		 (unsigned long long)l[RWA_CB_K_WAIT_BO], l[RWA_CB_K_WAIT_BO_NS] / 1e6, (unsigned long long)l[RWA_CB_K_CLOSE],
		 l[RWA_CB_K_CLOSE_NS] / 1e6);
	say(line);
	memset(&worst_cb, 0, sizeof(worst_cb));
}

/*
 * The kernel's boot-args, and after them the words of /a34/a34metal.args (A34): XNU keeps at most
 * 607 characters of boot-args (BOOT_LINE_LENGTH), and boot 65's line was cut there, after camtl=1 --
 * its camprof=5 never reached anybody. The driver's own switches (camprof=, rwm_*, metal_profile=,
 * camtl=, a34metalall=, ...) can live in that file instead, spaces or lines between them.
 */
extern "C" const char *
rwm_boot_args(void)
{
	static char arguments[2048];
	static int read;

	if (!read) {
		size_t size = 1024;
		FILE *more;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
			arguments[0] = '\0';
		}
		arguments[1024] = '\0';
		if ((more = fopen("/a34/a34metal.args", "r")) != NULL) {
			size_t at = strlen(arguments), n;

			arguments[at++] = ' ';
			n = fread(arguments + at, 1, sizeof(arguments) - at - 1, more);
			arguments[at + n] = '\0';
			for (size_t i = at; i < at + n; i++) {
				if (arguments[i] == '\n' || arguments[i] == '\r' || arguments[i] == '\t') {
					arguments[i] = ' ';
				}
			}
			fclose(more);
		}
		arguments[sizeof(arguments) - 1] = '\0';
		read = 1;
	}
	return arguments;
}

extern "C" bool
rwm_boot_arg(const char *text)
{
	return strstr(rwm_boot_args(), text) != nullptr;
}

extern "C" uint64_t
rwm_thread_cpu_ns(void)
{
	thread_basic_info_data_t info;
	mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
	mach_port_t self = mach_thread_self();
	uint64_t ns = 0;

	if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS) {
		ns = (uint64_t)(info.user_time.seconds + info.system_time.seconds) * 1000000000ull +
		     (uint64_t)(info.user_time.microseconds + info.system_time.microseconds) * 1000ull;
	}
	mach_port_deallocate(mach_task_self(), self);
	return ns;
}

extern "C" uint64_t
rwm_ns(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom;
}

extern "C" void
rwm_perf_report(uint64_t now)
{
	struct rwm_perf &p = rwm_perf;
	char line[320];
	const uint64_t span = now - p.since, cbs = p.cbs ? p.cbs : 1;

	if (rwm_camprof() == 0) {
		memset(&p, 0, sizeof(p));      /* (A34: off unless camprof=N) */
		return;
	}
	if (p.since == 0) {
		p.since = now;
		return;
	}
	if (span < rwm_camprof() * 1000000000ull) {
		return;
	}
	snprintf(line, sizeof(line), "rwm perf %llus: %llu cb (%.1f/s), %llu draws, cpu %.1f ms/cb, "
		 "gpu wait %.1f ms/cb, worst cb %.1f ms, %llu pipelines in %.0f ms; recording %.1f, between %.1f ms/cb; "
		 "%llu copied in, %llu KB, %.1f ms",
		 span / 1000000000ull, p.cbs, p.cbs * 1e9 / span, p.draws, p.encode / 1e6 / cbs, p.wait / 1e6 / cbs,
		 p.worst / 1e6, p.pipes, p.compile / 1e6, p.recording / 1e6 / cbs, p.between / 1e6 / cbs, p.copyins,
		 p.copyin_bytes >> 10, p.copyin_ns / 1e6);
	say(line);
	snprintf(line, sizeof(line), "rwm copied back (surfaces drawn by the GPU elsewhere, copied by the processor): "
		 "%llu, %llu KB, %.1f ms; per cb %.1f ms", p.copyouts, p.copyout_bytes >> 10, p.copyout_ns / 1e6,
		 p.copyout_ns / 1e6 / cbs);
	say(line);
	snprintf(line, sizeof(line), "rwm waits: queue full x%llu %.0f ms, until processed x%llu %.0f ms, "
		 "until completed x%llu %.0f ms (and on the completion queue x%llu %.0f ms); the worker %.1f ms a buffer",
		 p.queue_waits, p.queue_ns / 1e6, p.processed_waits, p.processed_ns / 1e6, p.completed_waits,
		 p.completed_ns / 1e6, p.completed_q_waits, p.completed_q_ns / 1e6,
		 p.worker_cbs ? p.worker_ns / 1e6 / p.worker_cbs : 0.0);
	{
		size_t at = strlen(line);

		snprintf(line + at, sizeof(line) - at, " (running %.1f of them)",
			 p.worker_cbs ? p.worker_cpu / 1e6 / p.worker_cbs : 0.0);
	}
	say(line);
	snprintf(line, sizeof(line), "rwm making: %llu buffers, %.1f ms each from made to committed, the maker running %.1f of them",
		 p.made_cbs, p.made_cbs ? p.made_ns / 1e6 / p.made_cbs : 0.0, p.made_cbs ? p.made_cpu / 1e6 / p.made_cbs : 0.0);
	say(line);
	snprintf(line, sizeof(line), "rwm frames in flight: %llu buffers begun while the GPU still drew the one committed before, "
		 "%llu after it had finished (%llu of them within 1 ms of its end as the completion queue saw it, %.1f ms "
		 "after it on average); handlers delivered: %llu scheduled, %llu completed", p.begun_overlapped,
		 p.begun_after_done, p.begun_soon, p.begun_gap_n ? p.begun_gap_ns / 1e6 / p.begun_gap_n : 0.0,
		 p.scheduled_handlers, p.completed_handlers);
	say(line);

	static void (*profile)(uint64_t *);
	static void (*submits)(uint64_t *);
	static int (*binds)(char *, size_t);
	static int (*pan)(unsigned, char *, size_t);
	static int looked;

	if (!looked) {
		looked = 1;
		profile = (void (*)(uint64_t *))profile_call("rwa_draw_profile");
		submits = (void (*)(uint64_t *))profile_call("rwa_submit_profile");
		binds = (int (*)(char *, size_t))profile_call("rwa_bind_profile");
		pan = (int (*)(unsigned, char *, size_t))profile_call("rwa_pan_profile");
	}
	if (submits != nullptr) {
		uint64_t sb[5] = { 0, 0, 0, 0, 0 };

		submits(sb);
		snprintf(line, sizeof(line), "rwm batches: %llu submitted, %llu us each; %llu blend shaders compiled; "
			 "%llu shader variants compiled in %llu ms", sb[0], sb[0] ? sb[1] / 1000 / sb[0] : 0, sb[2], sb[3],
			 sb[4] / 1000000);
		say(line);
	}
	if (profile != nullptr) {
		uint64_t d[7];

		profile(d);
		if (d[6] != 0) {
			snprintf(line, sizeof(line), "rwm draw us: check %llu, bindcheck %llu, state %llu, "
				 "resources %llu, draw_vbo %llu, unbind %llu (%llu draws)",
				 d[0] / 1000 / d[6], d[1] / 1000 / d[6], d[2] / 1000 / d[6], d[3] / 1000 / d[6],
				 d[4] / 1000 / d[6], d[5] / 1000 / d[6], d[6]);
			say(line);
		}
	}
	if (binds != nullptr && binds(line, sizeof(line)) != 0) {
		say(line);
	}
	say_worst_cb();
	for (unsigned which = 0; pan != nullptr && which < 47; which++) {    /* (4: tiles; 5: one span's jobs; 6 and on: its passes) */
		char wide[600];

		if (pan(which, wide, sizeof(wide)) != 0) {
			say(wide);
		}
	}
	memset(&p, 0, sizeof(p));
	p.since = now;
}
