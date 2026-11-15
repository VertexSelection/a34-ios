/*
 * A34: where the window server's two busy threads spend an animation burst, sampled (boot-arg rwm_sample=1 for 1000
 * samples a second, rwm_sample=N for N): CoreAnimation's render thread, the one that commits, and the driver's worker
 * (rwm_async=1). A thread of its own stops each of them N times a second (thread_suspend), reads where it is -- its pc
 * and lr and the return addresses up its frame-pointer chain -- and lets it go again; nothing that takes a lock is
 * called while one is stopped. Only samples taken while a burst is recorded (rwm_burst_on, cmdbuf.cpp) are kept, and
 * when the burst ends they are named (dladdr) and said as "rwm sample" lines: per thread, how often it was running,
 * the functions it was in (self) and those on its stack (inclusive), and by image. A sample costs the stopped thread
 * some 10 us; at 1000 a second that is about 1% of it.
 */
#include "rwm.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr unsigned kThreads = 4;
constexpr unsigned kDepth = 32;         /* pc, lr and 30 return addresses */
constexpr unsigned kSamples = 8192;     /* kept until a burst ends: some 4 s of two threads at 1000 a second */
constexpr unsigned kPcSlots = 1u << 16; /* address -> function, kept across bursts */
constexpr unsigned kFuncs = 8192;

struct watched {
	thread_act_t port;
	uintptr_t stack_lo, stack_hi;
	char role[16];
};

struct sample {
	uint8_t thread, running, depth;
	uintptr_t pc[kDepth];
};

struct func {
	uintptr_t start;        /* the symbol's address, or the address itself when there is none */
	const char *name;       /* dladdr's (the image's own string table), or NULL */
	const char *image;
	uintptr_t image_base;
};

watched threads[kThreads];
unsigned nthreads;
pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_once_t started = PTHREAD_ONCE_INIT;

sample *samples;
unsigned taken, missed;

struct pc_slot {
	uintptr_t pc;
	uint32_t func;
};
pc_slot *pc_slots;
func funcs[kFuncs];
unsigned nfuncs;
struct start_slot {
	uintptr_t start;
	uint32_t func;
};
start_slot *start_slots;

unsigned
rate(void)
{
	static int r = -1;

	if (r < 0) {
		int v = 0;

		if (strcmp(getprogname(), "backboardd") == 0) {
			if (const char *at = strstr(rwm_boot_args(), "rwm_sample=")) {
				long n = strtol(at + strlen("rwm_sample="), nullptr, 10);

				v = n <= 0 ? 0 : n < 10 ? 1000 : n > 5000 ? 5000 : (int)n;
			}
		}
		r = v;
	}
	return (unsigned)r;
}

void
say(const char *line)
{
	printf("redwoodmetal camprof: %s\n", line);
	fflush(stdout);
}

/* one sample of thread t: nothing but kernel calls and reads of its own stack while it is stopped */
void
take(unsigned t)
{
	watched *w = &threads[t];
	sample *s = &samples[taken];
	thread_basic_info_data_t info;
	mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;
	const bool running = thread_info(w->port, THREAD_BASIC_INFO, (thread_info_t)&info, &info_count) == KERN_SUCCESS &&
			     info.run_state == TH_STATE_RUNNING;
	arm_thread_state64_t state;
	mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
	unsigned depth = 0;

	if (thread_suspend(w->port) != KERN_SUCCESS) {
		missed++;
		return;
	}
	const kern_return_t got = thread_get_state(w->port, ARM_THREAD_STATE64, (thread_state_t)&state, &state_count);

	if (got == KERN_SUCCESS) {
		const uintptr_t sp = (uintptr_t)arm_thread_state64_get_sp(state);
		uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state);

		s->pc[depth++] = (uintptr_t)arm_thread_state64_get_pc(state);
		s->pc[depth++] = (uintptr_t)arm_thread_state64_get_lr(state);
		while (depth < kDepth && fp >= sp && fp >= w->stack_lo && fp + 16 <= w->stack_hi && (fp & 7) == 0) {
			const uintptr_t *frame = (const uintptr_t *)fp;
			const uintptr_t next = frame[0], ret = frame[1] & 0x0000ffffffffffffull;

			if (ret == 0) {
				break;
			}
			s->pc[depth++] = ret;
			if (next <= fp) {
				break;
			}
			fp = next;
		}
	}
	thread_resume(w->port);
	if (got != KERN_SUCCESS) {
		missed++;
		return;
	}
	s->thread = (uint8_t)t;
	s->running = running;
	s->depth = (uint8_t)depth;
	taken++;
}

const char *
base_name(const char *path)
{
	const char *slash = path != nullptr ? strrchr(path, '/') : nullptr;

	return slash != nullptr ? slash + 1 : path != nullptr ? path : "?";
}

/* the function an address is in (a return address is looked up one byte back, inside the call), or ~0u */
uint32_t
func_of(uintptr_t pc, bool ret)
{
	if (pc < 4096) {
		return ~0u;
	}
	const uintptr_t look = ret ? pc - 1 : pc;
	unsigned h = (unsigned)((look >> 2) * 2654435761u) & (kPcSlots - 1);

	for (unsigned probe = 0; probe < 64; probe++, h = (h + 1) & (kPcSlots - 1)) {
		if (pc_slots[h].pc == look) {
			return pc_slots[h].func;
		}
		if (pc_slots[h].pc == 0) {
			Dl_info info;
			uintptr_t start = look;
			const char *name = nullptr, *image = "?";
			uintptr_t base = 0;

			if (dladdr((const void *)look, &info) != 0) {
				image = base_name(info.dli_fname);
				base = (uintptr_t)info.dli_fbase;
				/* (a symbol 64 KB away is the shared cache's nearest exported one, not this function's) */
				if (info.dli_saddr != nullptr && info.dli_sname != nullptr &&
				    look - (uintptr_t)info.dli_saddr < 0x10000) {
					start = (uintptr_t)info.dli_saddr;
					name = info.dli_sname;
				}
			}
			if (name == nullptr) {
				start = look & ~(uintptr_t)0xfff;       /* (no symbol: one entry a page) */
			}
			unsigned g = (unsigned)((start >> 2) * 2654435761u) & (kFuncs * 2 - 1);
			uint32_t id = ~0u;

			for (unsigned k = 0; k < kFuncs * 2; k++, g = (g + 1) & (kFuncs * 2 - 1)) {
				if (start_slots[g].start == start) {
					id = start_slots[g].func;
					break;
				}
				if (start_slots[g].start == 0) {
					if (nfuncs < kFuncs) {
						id = nfuncs++;
						funcs[id] = { start, name, image, base };
						start_slots[g] = { start, id };
					}
					break;
				}
			}
			pc_slots[h] = { look, id };
			return id;
		}
	}
	return ~0u;
}

/* "name" short enough for a line: demangled, without its arguments, its last 44 characters */
void
func_name(uint32_t id, char *out, size_t size)
{
	const func &f = funcs[id];

	if (f.name == nullptr) {
		snprintf(out, size, "%.24s+%#lx", f.image, (unsigned long)(f.start - f.image_base));
		return;
	}
	int status = -1;
	char *plain = f.name[0] == '_' && f.name[1] == 'Z' ? abi::__cxa_demangle(f.name, nullptr, nullptr, &status) : nullptr;
	const char *name = status == 0 && plain != nullptr ? plain : f.name;
	size_t n = strcspn(name, "(");

	if (n > 44) {
		name += n - 44;
		n = 44;
	}
	snprintf(out, size, "%.*s", (int)n, name);
	free(plain);
}

/* the top `count` of `counts` (over nfuncs), said as lines of a few entries each */
void
say_top(const char *head, const unsigned *counts, unsigned total, unsigned count)
{
	unsigned order[48];
	unsigned n = 0;

	for (unsigned id = 0; id < nfuncs; id++) {
		if (counts[id] == 0) {
			continue;
		}
		unsigned at = n < count ? n++ : count;

		if (at == count && counts[id] <= counts[order[count - 1]]) {
			continue;
		}
		if (at == count) {
			at = count - 1;
		}
		while (at > 0 && counts[order[at - 1]] < counts[id]) {
			order[at] = order[at - 1];
			at--;
		}
		order[at] = id;
	}
	char line[240];
	size_t len = (size_t)snprintf(line, sizeof(line), "%s:", head);

	for (unsigned k = 0; k < n; k++) {
		char name[64], entry[96];
		int m;

		func_name(order[k], name, sizeof(name));
		m = snprintf(entry, sizeof(entry), " %s %.1f%%%s", name, 100.0 * counts[order[k]] / (total ? total : 1),
			     k + 1 < n ? "," : "");
		if (len + (size_t)m >= 220) {
			say(line);
			len = (size_t)snprintf(line, sizeof(line), "%s (more):", head);
		}
		memcpy(line + len, entry, (size_t)m + 1);
		len += (size_t)m;
	}
	say(line);
}

void
report(void)
{
	const unsigned n = __atomic_load_n(&nthreads, __ATOMIC_ACQUIRE);
	unsigned *self = (unsigned *)calloc(kFuncs, sizeof(unsigned));
	unsigned *incl = (unsigned *)calloc(kFuncs, sizeof(unsigned));
	uint32_t *seen = (uint32_t *)calloc(kDepth, sizeof(uint32_t));

	if (self == nullptr || incl == nullptr || seen == nullptr) {
		free(self);
		free(incl);
		free(seen);
		return;
	}
	for (unsigned t = 0; t < n; t++) {
		unsigned total = 0, running = 0;
		/* images on the stack, by name: a few, inclusive */
		const char *images[12] = { nullptr };
		unsigned image_counts[12] = { 0 };

		memset(self, 0, kFuncs * sizeof(unsigned));
		memset(incl, 0, kFuncs * sizeof(unsigned));
		for (unsigned i = 0; i < taken; i++) {
			const sample &s = samples[i];
			unsigned nseen = 0;
			const char *seen_images[12];
			unsigned nseen_images = 0;

			if (s.thread != t || s.depth == 0) {
				continue;
			}
			total++;
			running += s.running;
			const uint32_t leaf = func_of(s.pc[0], false);

			if (leaf != ~0u) {
				self[leaf]++;
			}
			for (unsigned d = 0; d < s.depth; d++) {
				uint32_t id = func_of(s.pc[d], d > 0);

				if (id == ~0u) {
					continue;
				}
				/* lr is a caller only when the function at pc made no frame: one already on the chain is skipped */
				if (d == 1 && s.depth > 2 && id == func_of(s.pc[2], true)) {
					continue;
				}
				bool again = false;

				for (unsigned k = 0; k < nseen; k++) {
					again = again || seen[k] == id;
				}
				if (again) {
					continue;
				}
				seen[nseen++] = id;
				incl[id]++;
				bool image_again = false;

				for (unsigned k = 0; k < nseen_images; k++) {
					image_again = image_again || strcmp(seen_images[k], funcs[id].image) == 0;
				}
				if (!image_again && nseen_images < 12) {
					seen_images[nseen_images++] = funcs[id].image;
					for (unsigned k = 0; k < 12; k++) {
						if (images[k] == nullptr) {
							images[k] = funcs[id].image;
						}
						if (strcmp(images[k], funcs[id].image) == 0) {
							image_counts[k]++;
							break;
						}
					}
				}
			}
		}
		if (total == 0) {
			continue;
		}
		char line[240], head[64];
		size_t len;

		snprintf(line, sizeof(line), "rwm sample %s: %u samples in the burst (%u a second asked; %u missed), running "
			 "in %.0f%% of them", threads[t].role, total, rate(), missed, 100.0 * running / total);
		say(line);
		len = (size_t)snprintf(line, sizeof(line), "rwm sample %s images:", threads[t].role);
		for (unsigned k = 0; k < 12 && images[k] != nullptr; k++) {
			int m = snprintf(line + len, sizeof(line) - len, " %.20s %.0f%%,", images[k],
					 100.0 * image_counts[k] / total);

			if (m < 0 || len + (size_t)m >= sizeof(line) - 1) {
				break;
			}
			len += (size_t)m;
		}
		say(line);
		snprintf(head, sizeof(head), "rwm sample %s self", threads[t].role);
		say_top(head, self, total, 16);
		snprintf(head, sizeof(head), "rwm sample %s inclusive", threads[t].role);
		say_top(head, incl, total, 40);
	}
	free(self);
	free(incl);
	free(seen);
}

void *
sampler_main(void *)
{
	mach_timebase_info_data_t tb;
	bool was_on = false;

	pthread_setname_np("rwm sampler");
	mach_timebase_info(&tb);
	const uint64_t period = 1000000000ull / rate() * tb.denom / tb.numer;
	uint64_t next = mach_absolute_time();

	for (;;) {
		const uint64_t now = mach_absolute_time();

		next += period;
		if (next < now) {
			next = now + period;
		}
		mach_wait_until(next);
		const bool on = __atomic_load_n(&rwm_burst_on, __ATOMIC_RELAXED);

		if (on) {
			const unsigned n = __atomic_load_n(&nthreads, __ATOMIC_ACQUIRE);

			for (unsigned t = 0; t < n && taken < kSamples; t++) {
				take(t);
			}
		} else if (was_on && taken != 0) {
			report();
			taken = 0;
			missed = 0;
		}
		was_on = on;
	}
	return nullptr;
}

void
start(void)
{
	pthread_attr_t attr;
	pthread_t t;

	samples = (sample *)calloc(kSamples, sizeof(sample));
	pc_slots = (pc_slot *)calloc(kPcSlots, sizeof(pc_slot));
	start_slots = (start_slot *)calloc(kFuncs * 2, sizeof(start_slot));
	if (samples == nullptr || pc_slots == nullptr || start_slots == nullptr) {
		return;
	}
	pthread_attr_init(&attr);
	pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &attr, sampler_main, nullptr) == 0) {
		char line[160];

		snprintf(line, sizeof(line), "rwm sample: sampling the render thread and the worker %u times a second "
			 "during animation bursts (rwm_sample)", rate());
		say(line);
	}
	pthread_attr_destroy(&attr);
}

} // namespace

/* A34: the calling thread, sampled during bursts as `role` (once a thread; boot-arg rwm_sample) */
extern "C" void
rwm_sample_thread(const char *role)
{
	static __thread bool registered;

	if (registered || rate() == 0) {
		return;
	}
	registered = true;
	pthread_t self = pthread_self();
	const uintptr_t hi = (uintptr_t)pthread_get_stackaddr_np(self);
	const size_t size = pthread_get_stacksize_np(self);

	pthread_mutex_lock(&reg_lock);
	const unsigned n = nthreads;

	if (n < kThreads) {
		watched *w = &threads[n];

		w->port = mach_thread_self();   /* (a send right of our own, kept) */
		w->stack_hi = hi;
		w->stack_lo = hi - size;
		snprintf(w->role, sizeof(w->role), "%s", role);
		__atomic_store_n(&nthreads, n + 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&reg_lock);
	pthread_once(&started, start);
}
