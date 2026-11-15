/*
 * tools/wifi-host-check: bsd/dev/a34_wifi.c (from xnu/patches/0939-a34-wifi-m1.patch) on a Linux host,
 * its registers and memory backed by plain buffers, its firmware read from FW_DIR: checks where the ROM-EMI
 * image, its date, the RAM code's regions and an INIT command land. No hardware is involved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "xnu_mock.h"

static const char *fw_dir;
vnode_t rootvnode = (vnode_t)1;
struct vnode { char path[512]; };
vfs_context_t vfs_context_kernel(void) { return NULL; }
kauth_cred_t vfs_context_ucred(vfs_context_t c) { (void)c; return NULL; }
proc_t vfs_context_proc(vfs_context_t c) { (void)c; return NULL; }
int vnode_lookup(const char *path, int f, vnode_t *vp, vfs_context_t c)
{
	(void)f; (void)c;
	const char *base = strrchr(path, '/');
	struct vnode *v = calloc(1, sizeof(*v));
	snprintf(v->path, sizeof(v->path), "%s%s", fw_dir, base);
	struct stat st;
	if (stat(v->path, &st) != 0) { free(v); return ENOENT; }
	*vp = v;
	return 0;
}
int vnode_getattr(vnode_t v, struct vnode_attr *a, vfs_context_t c) { (void)c; struct stat st; stat(v->path, &st); a->va_data_size = (uint64_t)st.st_size; return 0; }
int vnode_put(vnode_t v) { free(v); return 0; }
int vn_rdwr(enum uio_rw rw, vnode_t v, caddr_t base, int len, long long off, enum uio_seg seg, int io, kauth_cred_t cr, int *resid, proc_t p)
{
	(void)rw; (void)off; (void)seg; (void)io; (void)cr; (void)p;
	FILE *f = fopen(v->path, "rb"); size_t got = fread(base, 1, (size_t)len, f); fclose(f);
	*resid = len - (int)got; return 0;
}
void *kalloc_data(size_t n, int fl) { (void)fl; return malloc(n); }
void kfree_data(void *p, size_t n) { (void)n; free(p); }
void delay(int us) { (void)us; }
void delay_for_interval(uint32_t a, uint32_t b) { (void)a; (void)b; }
static uint64_t now;
uint64_t mach_absolute_time(void) { return now += 1000; }
void absolutetime_to_nanoseconds(uint64_t t, uint64_t *r) { *r = t; }
void lck_mtx_lock(lck_mtx_t *m) { (void)m; }
void lck_mtx_unlock(lck_mtx_t *m) { (void)m; }
wait_result_t lck_mtx_sleep(lck_mtx_t *m, lck_sleep_action_t a, event_t e, wait_interrupt_t i) { (void)m; (void)a; (void)e; (void)i; return 0; }
void thread_wakeup(event_t e) { (void)e; }
int PE_parse_boot_argn(const char *n, void *v, int s) { (void)n; (void)v; (void)s; return 0; }
int sysctl_io_number(struct sysctl_req *r, long long b, size_t s, void *p, int *c) { (void)r; (void)b; (void)s; (void)p; (void)c; return 0; }
kern_return_t kernel_thread_start(thread_continue_t f, void *a, thread_t *t) { (void)f; (void)a; (void)t; return 1; }
void thread_deallocate(thread_t t) { (void)t; }
void a34_pmic_lock(void) {}
void a34_pmic_unlock(void) {}
int ruby_pwrap_read(uintptr_t p, uint16_t r, uint16_t *v) { (void)p; (void)r; *v = 0; return 0; }
struct region { uintptr_t pa; size_t size; uint8_t *mem; } regions[8]; static int nregions;
/* a34_connsys.c (0960), as seen from Wi-Fi: CONNSYS up, its memory at the default 0x60000000 */
int a34_connsys_get(uint32_t radio) { (void)radio; return 0; }
bool a34_connsys_readable(const char *when) { (void)when; return true; }
uint64_t a34_connsys_emi_pa(void) { return 0x60000000ull; }
uint32_t a34_connsys_emi_size(void) { return 0x780000u; }
uint32_t a34_connsys_adie_id(void) { return 0x66358a10u; }
void sweet_log_flush(void) {}
vm_offset_t ml_io_map_unmappable(vm_offset_t pa, vm_size_t size, uint32_t flags)
{
	(void)flags;
	regions[nregions].pa = pa; regions[nregions].size = size; regions[nregions].mem = calloc(1, size);
	return (vm_offset_t)regions[nregions++].mem;
}

#include "a34_wifi.c"     /* (run.sh puts it beside ruby_hal.h) */

static uint8_t *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb"); fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc(*n); if (fread(d, 1, *n, f) != *n) abort(); fclose(f); return d;
}

int main(int argc, char **argv)
{
	char p[512];
	size_t n, m;
	int bad = 0;

	fw_dir = argv[1];
	(void)argc;
	if (!a34w_map() || !a34w_map_wf()) { puts("map failed"); return 1; }
	memset((uint8_t *)a34w_emi, 0xb7, 0x80000);     /* Bluetooth's images (a34_bt.c) must survive Wi-Fi's load */
	if (!a34w_rom_emi_load()) { puts("rom emi load failed"); return 1; }
	for (size_t i = 0; i < 0x80000; i++) { if (((uint8_t *)a34w_emi)[i] != 0xb7) { bad = 1; break; } }
	printf("Bluetooth's part of the EMI left alone: %s\n", bad ? "WRONG" : "ok");
	snprintf(p, sizeof(p), "%s/" FW_ROM_EMI, fw_dir);
	uint8_t *rom = slurp(p, &n);
	bad |= memcmp((uint8_t *)a34w_emi + 0x380000, rom + 48, n - 48) != 0;
	bad |= memcmp((uint8_t *)a34w_emi + 0x4954e0, rom, 16) != 0;
	printf("ROM EMI image at EMI+0x380000 and its date at +0x4954e0: %s\n", bad ? "WRONG" : "ok");

	if (!a34w_ram_code_load()) { puts("ram code load failed"); return 1; }
	snprintf(p, sizeof(p), "%s/" FW_RAM_CODE, fw_dir);
	uint8_t *ram = slurp(p, &m);
	/* regions 3/4 in the EMI, region 2 the last one through the window, the remap register its address */
	bad |= memcmp((uint8_t *)a34w_emi + 0x4ad400, ram + 0x8b00, 0xa4e70) != 0;
	bad |= memcmp((uint8_t *)a34w_emi + 0x5bd400, ram + 0xad970, 0x3f080) != 0;
	bad |= memcmp((uint8_t *)a34w_wf + WF_WINDOW, ram + 0x1360, 0x77a0) != 0;
	bad |= a34w_rd(a34w_wf, WF_AP2WF_REMAP_1) != 0x00404000u;
	printf("RAM code: EMI regions, window contents, remap register: %s\n", bad ? "WRONG" : "ok");

	/* an INIT command's frame as it lands in TX ring 17's slot 0 */
	a34w_wfdma_init();
	uint8_t payload[8] = { 0 };
	(void)a34w_init_cmd(INIT_CMD_ID_DYN_MEM_MAP_FW_FINISH, payload, sizeof(payload));
	uint8_t *f = (uint8_t *)a34w_dma + DMA_TX17_BUFS;
	volatile uint32_t *d = a34w_desc(DMA_TX17_DESC, 0);
	printf("FW_FINISH frame: DW0 0x%08x DW1 0x%08x | len %u cid 0x%02x type 0x%02x seq %u | desc 0x%08x 0x%08x | "
	    "TX17 base 0x%08x cnt %u cidx %u, RX0 cidx %u, GLO_CFG 0x%08x\n",
	    a34w_le32(f), a34w_le32(f + 4), f[32] | f[33] << 8, f[36], f[37], f[39], d[0], d[1],
	    a34w_rd(a34w_conn, WFDMA_TX_RING(17)), a34w_rd(a34w_conn, WFDMA_TX_RING(17) + 4), a34w_rd(a34w_conn, WFDMA_TX_RING(17) + 8),
	    a34w_rd(a34w_conn, WFDMA_RX_RING(0) + 8), a34w_rd(a34w_conn, WFDMA_GLO_CFG));
	bad |= a34w_le32(f) != ((72u) | (2u << 23)) || a34w_le32(f + 4) != (1u << 16) || f[36] != 0x41 || f[37] != 0xa0 ||
	    (f[32] | f[33] << 8) != 40 || d[0] != 0x58010000u || d[1] != ((72u << 16) | (1u << 30));
	bad |= a34w_rd(a34w_conn, WFDMA_GLO_CFG) != 0x50209075u;
	printf("frame and rings: %s\n", bad ? "WRONG" : "ok");
	printf("log:\n%s", a34w_log);
	return bad;
}
