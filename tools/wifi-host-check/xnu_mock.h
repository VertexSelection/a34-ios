/* Minimal stand-ins for the XNU declarations a34_wifi.c uses: a syntax and type check on Linux, not a build. */
#ifndef XNU_MOCK_H
#define XNU_MOCK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
/* Darwin arm64: uint64_t is unsigned long long (Linux x86_64: unsigned long), so %llx checks as on the real build */
typedef unsigned long long darwin_u64;
#define uint64_t darwin_u64
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef int wait_interrupt_t;
typedef void *event_t;
typedef struct thread *thread_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
typedef struct lck_grp lck_grp_t;
typedef struct lck_mtx { int x; } lck_mtx_t;
typedef unsigned int lck_sleep_action_t;
typedef long long off_t_;
typedef struct vnode *vnode_t;
typedef struct vfs_context *vfs_context_t;
typedef struct ucred *kauth_cred_t;
typedef struct proc *proc_t;
typedef char *caddr_t;
typedef uint64_t addr64_t;
#define KERN_SUCCESS 0
#define THREAD_UNINT 0
#define LCK_SLEEP_DEFAULT 0
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NULLVP ((vnode_t)0)
#define EINVAL 22
#define EIO 5
#define ENOMEM 12
#define ENOENT 2
#define EFBIG 27
#define ETIMEDOUT 60
#define EBUSY 16
#define EALREADY 37
#define __unused __attribute__((unused))
#define __printflike(a, b) __attribute__((format(printf, a, b)))
#define LCK_GRP_DECLARE(n, s) lck_grp_t *n
#define LCK_MTX_DECLARE(n, g) lck_mtx_t n
extern int printf(const char *, ...) __printflike(1, 2);
extern int snprintf(char *, size_t, const char *, ...) __printflike(3, 4);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);
extern void delay(int usec);
extern void delay_for_interval(uint32_t interval, uint32_t scale_factor);
extern uint64_t mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
extern void lck_mtx_lock(lck_mtx_t *);
extern void lck_mtx_unlock(lck_mtx_t *);
extern wait_result_t lck_mtx_sleep(lck_mtx_t *, lck_sleep_action_t, event_t, wait_interrupt_t);
extern wait_result_t lck_mtx_sleep_deadline(lck_mtx_t *, lck_sleep_action_t, event_t, wait_interrupt_t, uint64_t);
extern void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);   /* kern/clock.h */
extern void thread_wakeup(event_t);
extern int PE_parse_boot_argn(const char *, void *, int);
extern void *kalloc_data(size_t, int);
extern void read_random(void *, unsigned int);        /* bsd/sys/random.h */
extern void kfree_data(void *, size_t);
#define Z_WAITOK 0
#define Z_ZERO 0
/* vnode */
enum uio_rw { UIO_READ, UIO_WRITE };
enum uio_seg { UIO_SYSSPACE };
struct vnode_attr { uint64_t va_active; uint64_t va_data_size; };
#define VATTR_INIT(v) ((v)->va_active = 0)
#define VATTR_WANTED(v, a) ((v)->va_active |= 1)
extern vnode_t rootvnode;
extern vfs_context_t vfs_context_kernel(void);
extern kauth_cred_t vfs_context_ucred(vfs_context_t);
extern proc_t vfs_context_proc(vfs_context_t);
extern int vnode_lookup(const char *, int, vnode_t *, vfs_context_t);
extern int vnode_getattr(vnode_t, struct vnode_attr *, vfs_context_t);
extern int vnode_put(vnode_t);
extern int vn_rdwr(enum uio_rw, vnode_t, caddr_t, int, long long, enum uio_seg, int, kauth_cred_t, int *, proc_t);
/* sysctl */
struct sysctl_oid { int x; };
struct sysctl_req { int x; };
#define SYSCTL_HANDLER_ARGS (struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
#define SYSCTL_NODE(parent, nbr, name, access, handler, descr) int sysctl_node_##name
#define SYSCTL_PROC(parent, nbr, name, access, ptr, arg, handler, fmt, descr) \
	static int (*sysctl_proc_##name)SYSCTL_HANDLER_ARGS __attribute__((used)) = handler
#define SYSCTL_INT(parent, nbr, name, access, ptr, val, descr) static int *sysctl_int_##name __attribute__((used)) = ptr
#define SYSCTL_OUT(r, p, l) ((void)(r), (void)(p), (int)(l) * 0)
extern int sysctl_io_number(struct sysctl_req *req, long long bigValue, size_t valueSize, void *pValue, int *changed);
#define CTLTYPE_INT 2
#define CTLTYPE_STRING 3
#define CTLFLAG_RW 0
#define CTLFLAG_RD 0
#define CTLFLAG_LOCKED 0
#define OID_AUTO (-1)
/* machine */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
#endif
