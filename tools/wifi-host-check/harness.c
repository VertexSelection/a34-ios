/*
 * tools/wifi-host-check: bsd/dev/a34_wifi.c (from xnu/patches/0939-a34-wifi-m1.patch) on a Linux host,
 * its registers and memory backed by plain buffers, its firmware read from FW_DIR: checks where the ROM-EMI
 * image, its date, the RAM code's regions and an INIT command land. No hardware is involved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "xnu_mock.h"

static const char *fw_dir, *mac_path;     /* (0963) /a34/wifi/mac is mac_path */
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
	if (strcmp(base, "/mac") == 0) snprintf(v->path, sizeof(v->path), "%s", mac_path);
	else snprintf(v->path, sizeof(v->path), "%s%s", fw_dir, base);
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
void read_random(void *p, unsigned int n) { for (unsigned i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)(0x5d + 13 * i); }
static void firmware_sim(void);
void delay(int us) { (void)us; firmware_sim(); }
void delay_for_interval(uint32_t a, uint32_t b) { (void)a; (void)b; }
static uint64_t now;
uint64_t mach_absolute_time(void) { return now += 1000; }
void absolutetime_to_nanoseconds(uint64_t t, uint64_t *r) { *r = t; }
void lck_mtx_lock(lck_mtx_t *m) { (void)m; }
void lck_mtx_unlock(lck_mtx_t *m) { (void)m; }
wait_result_t lck_mtx_sleep(lck_mtx_t *m, lck_sleep_action_t a, event_t e, wait_interrupt_t i) { (void)m; (void)a; (void)e; (void)i; return 0; }
void thread_wakeup(event_t e) { (void)e; }
wait_result_t lck_mtx_sleep_deadline(lck_mtx_t *m, lck_sleep_action_t a, event_t e, wait_interrupt_t i, uint64_t d) { (void)m; (void)a; (void)e; (void)i; (void)d; return 0; }
void clock_interval_to_deadline(uint32_t i, uint32_t f, uint64_t *r) { *r = (uint64_t)i * f; }
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

/* (0965) bsd/dev/a34_wifi_net.c, the network interface, stood in for: what reaches it is recorded */
static uint8_t net_got[2048];
static uint32_t net_got_len, net_got_count;
static bool net_link;
int a34_wifi_net_attach(const uint8_t address[6], bool (*tx)(const uint8_t *, uint32_t));
void a34_wifi_net_input(const uint8_t *frame, uint32_t length);
void a34_wifi_net_link(bool up);
const char *a34_wifi_net_name(void);
int a34_wifi_net_attach(const uint8_t address[6], bool (*tx)(const uint8_t *, uint32_t)) { (void)address; (void)tx; return 0; }
void a34_wifi_net_input(const uint8_t *frame, uint32_t length) { if (length <= sizeof(net_got)) { memcpy(net_got, frame, length); net_got_len = length; } net_got_count++; }
void a34_wifi_net_link(bool up) { net_link = up; }
const char *a34_wifi_net_name(void) { return "en9"; }
/* (0969) the registry entries (iokit/Kernel/arm/A34WLAN.cpp) and the kernel's KPIs a34_apple80211.c uses, stood in for */
static char wlan_registered[16];
void a34_wlan_registry(const char *interface, const uint8_t *address);
void a34_wlan_registry(const char *interface, const uint8_t *address) { (void)address; snprintf(wlan_registered, sizeof(wlan_registered), "%s", interface); }
int copyout(const void *k, user_addr_t u, size_t n) { memcpy((void *)(uintptr_t)u, k, n); return 0; }
int copyin(user_addr_t u, void *k, size_t n) { memcpy(k, (const void *)(uintptr_t)u, n); return 0; }
void proc_selfname(char *b, int n) { snprintf(b, (size_t)n, "wifid"); }
static uint32_t kev_codes[64], kev_count;
int kev_post_msg(struct kev_msg *m) { if (kev_count < 64) kev_codes[kev_count++] = m->event_code; return 0; }
size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }
#include "a34_wifi.c"     /* (run.sh puts it beside ruby_hal.h) */
#include "a34_wpa.c"      /* (0964) */
#include "a34_apple80211.c"   /* (0969) */

/*
 * (0964) The firmware's side of the mailbox, simulated: whenever the code waits (delay), every
 * frame it put into the mailbox is taken and kept in `sent` (a copy each), as the firmware would.
 */
static int sim_on, sim_ap, sim_ap_ht;
/* (0968) an HT AP's HT Capabilities (MCS 0-15, 40 MHz, SGI 20/40) and WMM Parameter element (BE AIFSN 3 CW 15-1023, VO AIFSN 2 CW 3-7) */
static const uint8_t sim_ht_cap[28] = { 45, 26, 0xef, 0x09, 0x17, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t sim_ht_op[24] = { 61, 22, 48, 0x05, 0x04, 0x00, 0x00, 0x00 };
static const uint8_t sim_wmm[26] = { 221, 24, 0x00, 0x50, 0xf2, 2, 1, 1, 0x80, 0, 0x03, 0xa4, 0, 0, 0x27, 0xa4, 0, 0,
	                             0x42, 0x43, 0x5e, 0, 0x62, 0x32, 0x2f, 0 };
static uint8_t sent[64][CMD_FRAME_MOST];
static uint32_t sent_len[64], nsent;
static uint8_t sim_ap_addr[6];
/* puts `len` bytes into RX ring `ring`'s next slot (ring 0: the event ring; 2..5: the data rings) as the DMA would */
static uint32_t rx_w[6];
static bool rx_w_set[6];
static void rx_put(uint32_t ring, const uint8_t *data, uint32_t len)
{
	uint32_t slot, slots = ring == 0 ? RING_SLOTS : a34w_drx[ring - 2].slots;
	volatile uint32_t *d;
	uint8_t *buf;
	if (!rx_w_set[ring]) {          /* the DMA's next slot: the one after the host's */
		rx_w[ring] = ((ring == 0 ? a34w_rx_cidx : a34w_drx[ring - 2].cidx) + 1u) % slots;
		rx_w_set[ring] = true;
	}
	slot = rx_w[ring];
	rx_w[ring] = (slot + 1u) % slots;
	if (ring == 0) {
		d = a34w_desc(DMA_RX0_DESC, slot); buf = (uint8_t *)a34w_dma + DMA_RX0_BUFS + slot * SLOT_SIZE;
	} else {
		d = a34w_desc(DMA_RXN_DESC(ring), slot); buf = (uint8_t *)a34w_dma + DMA_RXN_BUFS(ring) + slot * SLOT_SIZE;
	}
	memcpy(buf, data, len);
	d[1] = (1u << 31) | (1u << 30) | (len << 16);
}
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
/* an event on RX ring 0: RXD (24), WIFI_EVENT header (12), body */
static void rx_event(uint8_t eid, uint8_t seq, const uint8_t *body, uint32_t n)
{
	uint8_t f[256] = { 0 };
	put32(f, 0x38000000u | (24u + 12u + n));
	f[24 + 4] = eid; f[24 + 5] = seq;
	memcpy(f + 36, body, n);
	rx_put(0, f, 36u + n);
}
/* a forwarded 802.11 management frame on RX ring 0: RXD (24), G3 (8, -50 dBm), the frame */
static void rx_mgmt(const uint8_t *frame, uint32_t n, uint8_t channel)
{
	uint8_t f[512] = { 0 };
	put32(f, 0x38010000u | (32u + n)); put32(f + 4, 1u << 13); put32(f + 12, (uint32_t)channel << 8);
	put32(f + 28, 120);
	memcpy(f + 32, frame, n);
	rx_put(0, f, 32u + n);
}
static uint32_t dw_of(const uint8_t *txd, int i) { return a34w_le32(txd + 4 * i); }

/*
 * (0964) The firmware's side of the mailbox, simulated: whenever the code waits (delay), every
 * frame it put into the mailbox is taken and kept in `sent` (a copy each), as the firmware would;
 * with sim_ap, the firmware and an AP answer: the channel granted (the request's token), the
 * Authentication answered (status 0) and acknowledged, the Association answered (AID 5).
 */
static void sim_react(const uint8_t *t)
{
	uint8_t ev[24], fr[64];
	bool cmd = ((dw_of(t, 1) >> 16) & 3u) == 1u;
	if (!sim_ap) return;
	bzero(ev, sizeof(ev)); bzero(fr, sizeof(fr));
	if (cmd && t[36] == 0x03) {                     /* (0969) a scan: the AP's beacon, then SCAN_DONE */
		uint8_t b[160], done[16];
		uint32_t k = 36;
		bzero(b, sizeof(b)); bzero(done, sizeof(done));
		b[0] = 0x80; memset(b + 4, 0xff, 6); memcpy(b + 10, sim_ap_addr, 6); memcpy(b + 16, sim_ap_addr, 6);
		b[32] = 100; b[34] = 0x01;
		b[k++] = 0; b[k++] = 9; memcpy(b + k, "Sim Open", 9); k += 9;      /* (9 bytes: "Sim Open" and a 0) */
		b[k++] = 1; b[k++] = 4; memcpy(b + k, "\x82\x84\x8b\x96", 4); k += 4;
		b[k++] = 3; b[k++] = 1; b[k++] = 6;
		rx_mgmt(b, k, 6);
		done[0] = t[64]; done[4] = 22;
		rx_event(EVENT_ID_SCAN_DONE, 0, done, 16);
	} else if (cmd && t[36] == 0x1c && t[64 + 2] == 0) {
		ev[0] = 0; ev[1] = t[64 + 1]; ev[2] = 0; ev[3] = t[64 + 3];
		rx_event(EVENT_ID_CH_PRIVILEGE, 0, ev, 24);
	} else if (!cmd && (t[32] >> 4) == 11) {
		fr[0] = 0xb0; memcpy(fr + 4, t + 32 + 10, 6); memcpy(fr + 10, sim_ap_addr, 6); memcpy(fr + 16, sim_ap_addr, 6);
		fr[26] = 2;
		rx_mgmt(fr, 30, 5);
		ev[0] = t[20]; ev[1] = 0; ev[5] = 1;    /* its TX status: acknowledged at the first try */
		rx_event(EVENT_ID_TX_DONE, 0, ev, 16);
	} else if (!cmd && (t[32] >> 4) == 0) {
		fr[0] = 0x10; memcpy(fr + 4, t + 32 + 10, 6); memcpy(fr + 10, sim_ap_addr, 6); memcpy(fr + 16, sim_ap_addr, 6);
		fr[24] = 0x21; fr[26] = 0; fr[28] = 0x05; fr[29] = 0xc0;   /* status 0, AID 5 (bits 14/15 set) */
		if (sim_ap_ht) {
			uint8_t big[30 + 28 + 24 + 26];
			memcpy(big, fr, 30); memcpy(big + 30, sim_ht_cap, 28); memcpy(big + 58, sim_ht_op, 24); memcpy(big + 82, sim_wmm, 26);
			rx_mgmt(big, sizeof(big), 48);
		} else {
			rx_mgmt(fr, 30, 5);
		}
	}
}
static void firmware_sim(void)
{
	volatile uint32_t *drv, *fw;
	if (!sim_on || !a34w_mbox_ready) return;
	drv = a34w_mbox_word(0); fw = a34w_mbox_word(4);
	while (*fw != *drv) {
		const uint8_t *slot = (const uint8_t *)(a34w_emi + a34w_mbox_offset + 8u + *fw * CMD_FRAME_MOST);
		uint32_t len = a34w_le32(slot) & 0xffffu;
		if (nsent < 64) { memcpy(sent[nsent], slot, len > CMD_FRAME_MOST ? CMD_FRAME_MOST : len); sent_len[nsent++] = len; }
		*fw = (*fw + 1u) % SW_WFDMA_SLOTS;
		sim_react(slot);
	}
}

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

	/* ---- milestone 2 (0961): the mailbox, a command in it, RX ring 0's events and forwarded frames ---- */
	*(volatile uint32_t *)(a34w_conn + SYSRAM_SW_WFDMA_OFFSET) = 0x00713400u;   /* what the ROM left on the phone */
	bad |= !a34w_mbox_reset() || a34w_mbox_offset != 0x713400u;
	bad |= !a34w_bss_activate(true);
	{
		uint8_t *m = (uint8_t *)a34w_emi + 0x713400u, *c = m + 8;       /* slot 0 */
		printf("BSS_ACTIVATE in mailbox slot 0: DW0 0x%08x len %u cid 0x%02x type 0x%02x set %u seq %u | active %u "
		    "mac %02x:..:%02x bmc %u | drv %u, CCIF TCHNUM %u\n", a34w_le32(c), c[32] | c[33] << 8, c[36], c[37], c[38],
		    c[39], c[65], c[68], c[73], c[74], a34w_le32(m), a34w_rd(a34w_conn, CCIF_TCHNUM));
		bad |= a34w_le32(c) != (0x01000000u | 76u) || (c[32] | c[33] << 8) != 44 || c[36] != 0x11 || c[37] != 0xa0 ||
		    c[38] != 1 || c[65] != 1 || memcmp(c + 68, a34w_mac, 6) != 0 || c[74] != 4 || a34w_le32(m) != 1 ||
		    a34w_rd(a34w_conn, CCIF_TCHNUM) != 3;
	}
	/* RX ring 0: a forwarded beacon (SW kind 0x3801, group 3 with RCPI 120 = -50 dBm, channel 6), then an event */
	{
		uint32_t slot = (a34w_rx_cidx + 1u) % RING_SLOTS;
		uint8_t *buf = (uint8_t *)a34w_dma + DMA_RX0_BUFS + slot * SLOT_SIZE, *bc;
		volatile uint32_t *dsc = a34w_desc(DMA_RX0_DESC, slot);
		uint32_t len = 24 + 8 + 24 + 12 + 2 + 5 + 3, w;
		memset(buf, 0, SLOT_SIZE);
		w = 0x38010000u | len; memcpy(buf, &w, 4);
		w = 1u << 13; memcpy(buf + 4, &w, 4);                   /* G3 only */
		w = 6u << 8; memcpy(buf + 12, &w, 4);                   /* channel 6 */
		w = 0; memcpy(buf + 24, &w, 4); w = 120; memcpy(buf + 28, &w, 4);
		bc = buf + 32;
		bc[0] = 0x80; memset(bc + 4, 0xff, 6); memcpy(bc + 10, "\x0a\x0b\x0c\x0d\x0e\x0f", 6); memcpy(bc + 16, "\x0a\x0b\x0c\x0d\x0e\x0f", 6);
		bc[32] = 100; bc[34] = 0x01;
		bc[36] = 0; bc[37] = 5; memcpy(bc + 38, "hello", 5); bc[43] = 3; bc[44] = 1; bc[45] = 6;
		dsc[1] = (1u << 31) | (len << 16);
		slot = (slot + 1u) % RING_SLOTS;
		buf = (uint8_t *)a34w_dma + DMA_RX0_BUFS + slot * SLOT_SIZE; dsc = a34w_desc(DMA_RX0_DESC, slot);
		len = 24 + 12 + 476;
		memset(buf, 0, SLOT_SIZE);
		w = 0x38000000u | len; memcpy(buf, &w, 4);
		buf[24 + 4] = EVENT_ID_SCAN_DONE; buf[24 + 5] = 77; buf[36] = 9; buf[40] = 22;
		dsc[1] = (1u << 31) | (len << 16);
		uint8_t done[476]; int got = a34w_wait_event(EVENT_ID_SCAN_DONE, 0, done, sizeof(done), 100);
		printf("RX ring 0: SCAN_DONE %d bytes (scan %u, %u channels); %u networks: %02x:..:%02x ch %u %d dBm \"%s\"\n", got,
		    done[0], done[4], a34w_heard_count, a34w_heard[0].bssid[0], a34w_heard[0].bssid[5], a34w_heard[0].channel,
		    a34w_heard[0].rssi, a34w_heard[0].ssid);
		bad |= got != 476 || done[0] != 9 || done[4] != 22 || a34w_heard_count != 1 || a34w_heard[0].rssi != -50 ||
		    a34w_heard[0].channel != 6 || strcmp(a34w_heard[0].ssid, "hello") != 0;
	}
	printf("milestone 2 paths: %s\n", bad ? "WRONG" : "ok");

	/* ---- 0963: the phone's own address, /a34/wifi/mac ---- */
	{
		static const struct { const char *text; const char *want; bool efuse; } cases[] = {
			{ NULL, "5e:6a:77:84:91:9e", false },                   /* no file: random (read_random), made local */
			{ NULL, "00:11:22:33:44:55", true },                    /* no file, an efuse address: the chip's */
			{ "06:a1:b2:c3:d4:e5\n", "06:a1:b2:c3:d4:e5", true },   /* the kept one wins */
			{ "  0a-1b-2c-3d-4e-5f \r\n", "0a:1b:2c:3d:4e:5f", false },
			{ "03:11:22:33:44:55\n", "5e:6a:77:84:91:9e", false },   /* multicast and local: refused */
			{ "04:11:22:33:44:55\n", "00:11:22:33:44:55", true },    /* not locally administered: refused */
			{ "06:11:22:33:44:5\n", "5e:6a:77:84:91:9e", false },    /* short */
			{ "06:11:22:33:44:55 x\n", "5e:6a:77:84:91:9e", false }, /* more on the line */
		};
		static const uint8_t efuse[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
		char path[600], got[24];
		int failed = 0;

		snprintf(path, sizeof(path), "/tmp/a34wifi-host-check-mac-%d", (int)getpid());
		mac_path = path;
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			const char *source;

			unlink(path);
			if (cases[i].text != NULL) {
				FILE *mf = fopen(path, "w"); fputs(cases[i].text, mf); fclose(mf);
			}
			source = a34w_mac_choose(cases[i].efuse ? efuse : NULL);
			snprintf(got, sizeof(got), "%02x:%02x:%02x:%02x:%02x:%02x", a34w_mac[0], a34w_mac[1], a34w_mac[2], a34w_mac[3],
			    a34w_mac[4], a34w_mac[5]);
			if (strcmp(got, cases[i].want) != 0) {
				printf("MAC case %zu: %s (%s), wanted %s\n", i, got, source, cases[i].want);
				failed = 1;
			}
		}
		unlink(path);
		printf("0963 kept MAC address: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}
	/* ---- 0964: the network file ---- */
	{
		static const struct { const char *text; const char *why; bool psk; uint32_t ssid; } nets[] = {
			{ "ssid=Home Net\npsk=correct horse\n", NULL, true, 8 },
			{ "# mine\r\nssid=x\r\n", NULL, false, 1 },
			{ "ssid=a\npsk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n", NULL, true, 1 },
			{ "ssid=a\npsk=short\n", "psk= is neither 8 to 63 characters nor 64 hex digits", false, 0 },
			{ "psk=12345678\n", "no ssid= line", false, 0 },
			{ "ssid=a\nfoo=b\n", "a line that is not ssid=, psk= or bssid=", false, 0 },
			{ "ssid=a\nbssid=02:00:00:00:00:01\n", NULL, false, 1 },
		};
		int failed = 0;
		for (size_t i = 0; i < sizeof(nets) / sizeof(nets[0]); i++) {
			struct a34w_network n;
			const char *why = a34w_network_parse((const uint8_t *)nets[i].text, (uint32_t)strlen(nets[i].text), &n);
			if ((why == NULL) != (nets[i].why == NULL) || (why != NULL && strcmp(why, nets[i].why) != 0) ||
			    (why == NULL && (n.psk_given != nets[i].psk || n.ssid_length != nets[i].ssid))) {
				printf("network file case %zu: %s\n", i, why ? why : "(good)"); failed = 1;
			}
		}
		printf("0964 network file: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}

	/* ---- 0964: a whole open join against a simulated AP and firmware ---- */
	{
		static const uint8_t ap[6] = { 0xc4, 0x9a, 0x31, 0x2a, 0xc8, 0x59 };
		static const uint8_t own[6] = { 0x06, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5 };
		uint8_t fr[256];
		int failed = 0;
		uint32_t k = 0, auth_at = 0, assoc_at = 0;

		memcpy(a34w_mac, own, 6);
		bzero(&a34w_net, sizeof(a34w_net));
		memcpy(a34w_net.ssid, "veryCoolNetwork", 15); a34w_net.ssid_length = 15;
		bzero(&a34w_target, sizeof(a34w_target)); a34w_target.rssi = -128;
		a34w_joining = true;
		/* a beacon: SSID, rates 1 2 5.5 11 (basic) 6 9 12 18, DS channel 5, TIM (DTIM period 3), ext rates */
		bzero(fr, sizeof(fr));
		fr[0] = 0x80; memset(fr + 4, 0xff, 6); memcpy(fr + 10, ap, 6); memcpy(fr + 16, ap, 6);
		fr[32] = 100; fr[34] = 0x21; fr[35] = 0x04;    /* beacon interval 100, ESS + short preamble + short slot */
		k = 36;
		fr[k++] = 0; fr[k++] = 15; memcpy(fr + k, "veryCoolNetwork", 15); k += 15;
		fr[k++] = 1; fr[k++] = 8; memcpy(fr + k, "\x82\x84\x8b\x96\x0c\x12\x18\x24", 8); k += 8;
		fr[k++] = 3; fr[k++] = 1; fr[k++] = 5;
		fr[k++] = 5; fr[k++] = 4; fr[k++] = 0; fr[k++] = 3; fr[k++] = 0; fr[k++] = 0;
		fr[k++] = 50; fr[k++] = 4; memcpy(fr + k, "\x30\x48\x60\x6c", 4); k += 4;
		a34w_mgmt_in(fr, k, -70, 5);
		a34w_joining = false;
		failed |= !a34w_target.found || a34w_target.channel != 5 || a34w_target.rssi != -70;

		*a34w_mbox_word(4) = *a34w_mbox_word(0);        /* (what earlier checks left in the mailbox: taken) */
		nsent = 0; sim_on = 1; sim_ap = 1; memcpy(sim_ap_addr, ap, 6);
		bool joined = a34w_connect();
		firmware_sim();                                 /* (the last command, which nothing waited after) */
		sim_on = 0; sim_ap = 0;
		printf("0964 open join: %s, AID %u, %u frames through the mailbox:\n", joined ? "joined" : "NOT JOINED", a34w_aid, nsent);
		for (uint32_t i = 0; i < nsent; i++) {
			const uint8_t *t = sent[i];
			bool cmd = ((dw_of(t, 0) >> 23) & 3u) == 2u && ((dw_of(t, 1) >> 16) & 3u) == 1u;
			printf("  %2u: %4u bytes DW0 0x%08x DW1 0x%08x %s 0x%02x\n", i, sent_len[i], dw_of(t, 0), dw_of(t, 1),
			    cmd ? "command" : "frame, subtype", cmd ? t[36] : t[32] >> 4);
			if (!cmd && (t[32] >> 4) == 11) auth_at = i;
			if (!cmd && (t[32] >> 4) == 0) assoc_at = i;
		}
		/* the order: CH_PRIVILEGE, STA_REC (state 1), Authentication, Association Request, BSS_INFO, STA_REC
		 * (state 3), PM_BSS_CONNECTED, POWER_SAVE_MODE, RX filter, CH_PRIVILEGE abort */
		static const int want[] = { 0x1c, 0x13, -11, -0, 0x12, 0x13, 0x16, 0x05, 0x0a, 0x1c };
		failed |= !joined || a34w_aid != 5 || nsent != sizeof(want) / sizeof(want[0]);
		for (uint32_t i = 0; i < nsent && i < sizeof(want) / sizeof(want[0]); i++) {
			const uint8_t *t = sent[i];
			bool cmd = ((dw_of(t, 1) >> 16) & 3u) == 1u;
			if (want[i] > 0 ? (!cmd || t[36] != want[i]) : (cmd || (t[32] >> 4) != -want[i])) { failed = 1; printf("  frame %u is not the one expected\n", i); }
		}
		/* the Authentication frame's TXD, against the one nic_txd_v2_compose makes (WTBL 5, SN 1, PID 1) */
		{
			const uint8_t *t = sent[auth_at];
			uint32_t w[8]; for (int i = 0; i < 8; i++) w[i] = dw_of(t, i);
			printf("  Authentication TXD: %08x %08x %08x %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
			failed |= w[0] != 0x0310003eu || w[1] != 0x80026405u || w[2] != 0x801e000bu || (w[3] & 0x8000ffffu) != 0x8000f000u ||
			    (w[5] & 0x2ffu) != 0x201u || w[6] != 0 || w[7] != 0x000bc000u || memcmp(t + 32 + 4, ap, 6) != 0 ||
			    memcmp(t + 32 + 10, own, 6) != 0 || t[32 + 26] != 1;
		}
		/* the Association Request: capability ESS + short preamble + short slot, listen 10, SSID, the AP's rates, no RSN */
		{
			const uint8_t *f = sent[assoc_at] + 32;
			uint32_t n = sent_len[assoc_at] - 32;
			failed |= f[24] != 0x21 || f[25] != 0x04 || f[26] != 10 || f[28] != 0 || f[29] != 15 || memcmp(f + 30, "veryCoolNetwork", 15) != 0 ||
			    f[45] != 1 || f[46] != 8 || f[55] != 50 || n != 24 + 4 + 17 + 10 + 6 + 10 ||   /* (0968: + Extended Capabilities) */
			    f[61] != 127 || f[63 + 2] != 0x08;
		}
		/* STA_REC state 3: 168 bytes, AID 5, WTBL 5, broadcast 0xff, rates 0x0fcf? (1 2 5.5 11 6 9 12 18 24 36 48 54) basic 0x000f */
		{
			const uint8_t *c = sent[5] + 64;
			uint32_t operational = (uint32_t)(c[14] | c[15] << 8), basic = (uint32_t)(c[16] | c[17] << 8);
			printf("  STA_REC: %u bytes, state %u, AID %u, WTBL %u, phy 0x%02x, rates 0x%04x basic 0x%04x\n", sent_len[5] - 64,
			    c[20], c[8] | c[9] << 8, c[54], c[13], operational, basic);
			failed |= sent_len[5] != 64 + 168 || c[20] != 2 || (c[8] | c[9] << 8) != 5 || c[54] != 5 || c[55] != 0xff ||
			    c[13] != 0x03 || operational != 0x3fcfu || basic != 0x000fu || c[23] != 0;
		}
		/* BSS_INFO: 116 bytes, connected, BSSID, auth open, enc none, BMC 4, RLM channel 5 */
		{
			const uint8_t *c = sent[4] + 64;
			failed |= sent_len[4] != 64 + 116 || c[1] != 0 || memcmp(c + 36, ap, 6) != 0 || c[53] != 0 || c[54] != 1 ||
			    c[58] != 4 || c[68 + 2] != 5 || c[68 + 1] != 1 || c[48] != 0;
		}
		printf("0964 open join against a simulated AP: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}

	/* ---- 0964: EAPOL in on RX ring 2 (header translated), message 2 out on TX ring 0, the token back on ring 4 ---- */
	{
		uint8_t f[512], *m;
		int failed = 0;
		uint32_t n = 0;
		const uint8_t *tok = (const uint8_t *)a34w_dma + DMA_TOKENS;

		a34w_net.psk_given = true;
		a34w_associated = true;
		bzero(&a34w_wpa, sizeof(a34w_wpa));
		memset(a34w_wpa.pmk, 0x11, 32); memcpy(a34w_wpa.own, a34w_mac, 6); memcpy(a34w_wpa.peer, a34w_target.bssid, 6);
		memset(a34w_wpa.own_nonce, 0x22, 32); a34w_wpa.rsn = a34w_own_rsn; a34w_wpa.rsn_length = sizeof(a34w_own_rsn);
		bzero(f, sizeof(f));
		put32(f, (2u << 27) | (24u + 14u + 99u));      /* RXD: packet type 2 (data), byte count */
		put32(f + 4, AP_WLAN_INDEX);                    /* no groups */
		put32(f + 8, 1u << 13);                         /* header translated */
		m = f + 24;
		memcpy(m, a34w_mac, 6); memcpy(m + 6, a34w_target.bssid, 6); m[12] = 0x88; m[13] = 0x8e;
		m += 14;                                        /* EAPOL-Key, message 1 of 4 (RSN, version 2, pairwise, ack) */
		m[0] = 2; m[1] = 3; m[2] = 0; m[3] = 95; m[4] = 2; m[5] = 0x00; m[6] = 0x8a; m[8] = 16; m[16] = 1;
		memset(m + 17, 0x33, 32);
		n = 24u + 14u + 99u;
		rx_put(2, f, n);
		(void)a34w_data_poll();
		{
			uint32_t w0 = a34w_le32(tok), w1 = a34w_le32(tok + 4);
			const uint8_t *e = tok + 64;
			printf("0964 EAPOL: message 1 in -> %u out; TX0 cidx %u; token 0: DW0 0x%08x DW1 0x%08x TXP %02x%02x ptr 0x%08x len 0x%04x; "
			    "to %02x:..:%02x type %02x%02x key info %02x%02x\n", a34w_eapol_tx, a34w_rd(a34w_conn, WFDMA_TX_RING(0) + 8),
			    w0, w1, tok[33], tok[32], a34w_le32(tok + 40), tok[44] | tok[45] << 8, e[0], e[5], e[12], e[13], e[14 + 5], e[14 + 6]);
			failed |= a34w_eapol_tx != 1 || a34w_rd(a34w_conn, WFDMA_TX_RING(0) + 8) != 1 || (w0 & 0xffff0000u) != 0x020a0000u ||
			    w1 != 0x80008405u || tok[33] != 0x80 || a34w_le32(tok + 40) != 0x58140040u || e[12] != 0x88 || e[13] != 0x8e ||
			    memcmp(e, a34w_target.bssid, 6) != 0 || e[14 + 5] != 0x01 || e[14 + 6] != 0x0a || (a34w_tokens_busy[0] & 1u) == 0;
		}
		/* the token's report on RX ring 4 (version 3: one word, token 0 in bits 30:16) */
		bzero(f, sizeof(f));
		put32(f, (6u << 27) | (1u << 16) | 12u); put32(f + 4, 3u << 16); put32(f + 8, (0u << 16) | 1u);
		rx_put(4, f, 12);
		(void)a34w_data_poll();
		failed |= (a34w_tokens_busy[0] & 1u) != 0 || a34w_tokens_back != 1;
		printf("0964 data path (EAPOL out on TX ring 0, the token back on ring 4): %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}
	/* ---- 0965: step 9, the interface: an 802.11 data frame up (not translated: LLC/SNAP), an IP frame down ---- */
	{
		uint8_t f[512], ip[100];
		int failed = 0;
		const uint8_t *tok;
		uint32_t at;

		a34w_joined = true;
		a34w_keys_in = true;
		failed |= !a34w_step9() || !net_link;
		bzero(f, sizeof(f));
		put32(f, (2u << 27) | (24u + 24u + 8u + 20u));
		put32(f + 4, AP_WLAN_INDEX);
		put32(f + 8, 12u << 8);                         /* header length 24 (12 words of 2), not translated */
		f[24] = 0x08; f[25] = 0x02;                     /* data, FromDS: A1 DA, A2 BSSID, A3 SA */
		memcpy(f + 24 + 4, a34w_mac, 6); memcpy(f + 24 + 10, a34w_target.bssid, 6);
		memcpy(f + 24 + 16, "\x02\x99\x88\x77\x66\x55", 6);
		memcpy(f + 48, "\xaa\xaa\x03\x00\x00\x00\x08\x00", 8);
		memset(f + 56, 0x45, 20);
		rx_put(2, f, 24u + 24u + 8u + 20u);
		(void)a34w_data_poll();
		printf("0965 up: %u frames to the interface, %u bytes: to %02x:..:%02x from %02x:..:%02x type %02x%02x\n", net_got_count,
		    net_got_len, net_got[0], net_got[5], net_got[6], net_got[11], net_got[12], net_got[13]);
		failed |= net_got_count != 1 || net_got_len != 14 + 20 || memcmp(net_got, a34w_mac, 6) != 0 ||
		    memcmp(net_got + 6, "\x02\x99\x88\x77\x66\x55", 6) != 0 || net_got[12] != 0x08 || net_got[13] != 0x00 || net_got[14] != 0x45;
		/* down: an IPv4 frame from the stack, ciphered (keys in): token 1 (0 is busy again? it came back), protected bit */
		bzero(ip, sizeof(ip));
		memcpy(ip, a34w_target.bssid, 6); memcpy(ip + 6, a34w_mac, 6); ip[12] = 0x08; ip[14] = 0x45;
		at = a34w_dtx_cidx;
		failed |= !a34w_net_tx(ip, sizeof(ip));
		tok = (const uint8_t *)a34w_dma + DMA_TOKENS;     /* token 0 was given back: it is used again */
		printf("0965 down: TX0 cidx %u -> %u, TXD DW0 0x%08x DW1 0x%08x DW3 0x%08x DW5 0x%08x\n", at, a34w_dtx_cidx, a34w_le32(tok),
		    a34w_le32(tok + 4), a34w_le32(tok + 12), a34w_le32(tok + 20));
		failed |= a34w_le32(tok) != (0x020a0000u | (32u + sizeof(ip))) || a34w_le32(tok + 4) != 0x80008405u ||
		    a34w_le32(tok + 12) != 0x0000f002u || a34w_le32(tok + 20) != 0 || memcmp(tok + 64, ip, sizeof(ip)) != 0;
		printf("0965 interface data path: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}
	/* ---- 0967: an ADDBA request declined, a DHCP DISCOVER out with its TX status, the AP letting go ---- */
	{
		uint8_t f[64], dhcp[14 + 20 + 8 + 240 + 4];
		const uint8_t *t, *tok;
		int failed = 0;
		uint32_t n0, l4 = 14 + 20;

		*a34w_mbox_word(4) = *a34w_mbox_word(0);
		nsent = 0; sim_on = 1;
		a34w_joined = true; a34w_keys_in = true;
		bzero(f, sizeof(f));
		f[0] = 0xd0; memcpy(f + 4, a34w_mac, 6); memcpy(f + 10, a34w_target.bssid, 6); memcpy(f + 16, a34w_target.bssid, 6);
		f[24] = 3; f[25] = 0; f[26] = 7;                /* block ack, ADDBA Request, token 7 */
		f[27] = 0x02; f[28] = 0x10;                     /* BA parameter set: immediate, TID 0, 64 frames */
		a34w_mgmt_in(f, 33, -60, 48);
		firmware_sim();
		t = sent[0];
		printf("0967 ADDBA: %u frames to the firmware; the first: subtype %u, category %u action %u token %u status %u\n", nsent,
		    t[32] >> 4, t[32 + 24], t[32 + 25], t[32 + 26], t[32 + 27] | t[32 + 28] << 8);
		failed |= nsent != 1 || (t[32] >> 4) != 13 || t[32 + 24] != 3 || t[32 + 25] != 1 || t[32 + 26] != 7 || t[32 + 27] != 37 ||
		    (dw_of(t, 2) & 0xfu) != 13u;
		/* DHCP DISCOVER from the stack: UDP 68 -> 67, option 53 = 1 */
		bzero(dhcp, sizeof(dhcp));
		memset(dhcp, 0xff, 6); memcpy(dhcp + 6, a34w_mac, 6); dhcp[12] = 0x08; dhcp[14] = 0x45; dhcp[14 + 9] = 17;
		memset(dhcp + 14 + 16, 0xff, 4);
		dhcp[l4 + 1] = 68; dhcp[l4 + 3] = 67;
		dhcp[l4 + 8 + 236] = 99; dhcp[l4 + 8 + 237] = 130; dhcp[l4 + 8 + 238] = 83; dhcp[l4 + 8 + 239] = 99;
		dhcp[l4 + 8 + 240] = 53; dhcp[l4 + 8 + 241] = 1; dhcp[l4 + 8 + 242] = 1; dhcp[l4 + 8 + 243] = 255;
		n0 = a34w_dtx_cidx;
		failed |= !a34w_net_tx(dhcp, sizeof(dhcp));
		tok = (const uint8_t *)a34w_dma + DMA_TOKENS;
		for (uint32_t k = 0; k < DATA_TX_SLOTS; k++) {         /* the token buffer that holds it */
			const uint8_t *b = (const uint8_t *)a34w_dma + DMA_TOKENS + k * DATA_TOKEN_SIZE;
			if (memcmp(b + 64, dhcp, sizeof(dhcp)) == 0) { tok = b; break; }
		}
		printf("0967 DHCP out: TX0 %u -> %u, DW3 0x%08x (protected bit %u), DW5 0x%08x (TX status asked for: %s)\n", n0, a34w_dtx_cidx,
		    a34w_le32(tok + 12), (a34w_le32(tok + 12) >> 1) & 1u, a34w_le32(tok + 20), (a34w_le32(tok + 20) & 0x200u) ? "yes" : "no");
		failed |= memcmp(tok + 64, dhcp, sizeof(dhcp)) != 0 || (a34w_le32(tok + 12) & 2u) == 0 || (a34w_le32(tok + 20) & 0x2ffu) == 0 ||
		    strstr(a34w_log, "DHCP DISCOVER") == NULL;
		/* the AP disassociates us: the firmware's side undone, a join again in 3 s */
		nsent = 0;
		bzero(f, sizeof(f));
		f[0] = 0xa0; memcpy(f + 4, a34w_mac, 6); memcpy(f + 10, a34w_target.bssid, 6); memcpy(f + 16, a34w_target.bssid, 6);
		f[24] = 37;
		a34w_mgmt_in(f, 26, -60, 48);
		firmware_sim();
		printf("0967 LEFT: joined %d, rejoin in %lld ms, %u commands (", a34w_joined,
		    (long long)a34w_auto_rejoin_at - (long long)a34w_ms(), nsent);
		for (uint32_t i = 0; i < nsent; i++) printf(" 0x%02x", sent[i][36]);
		printf(" )\n");
		failed |= a34w_joined || a34w_auto_rejoin_at == 0 || nsent < 3 || sent[0][36] != 0x17 || sent[1][36] != 0x12 || sent[2][36] != 0x14 ||
		    net_link;
		sim_on = 0;
		printf("0967 action frames, DHCP TX status, leaving: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}

	/* ---- 0968: a join with an HT + WMM AP on 5 GHz; a BSS transition request answered ---- */
	{
		static const uint8_t ap[6] = { 0xc4, 0x9a, 0x31, 0x2a, 0xc8, 0x5a };
		uint8_t fr[256], ies[128];
		int failed = 0;
		uint32_t k = 0, n;
		int assoc_at = -1, wmm_at = -1, bss_at = -1, sta3_at = -1;

		bzero(&a34w_target, sizeof(a34w_target)); a34w_target.rssi = -128;
		a34w_joining = true;
		bzero(fr, sizeof(fr));
		fr[0] = 0x80; memset(fr + 4, 0xff, 6); memcpy(fr + 10, ap, 6); memcpy(fr + 16, ap, 6);
		fr[32] = 100; fr[34] = 0x11; fr[35] = 0x19;
		k = 36;
		fr[k++] = 0; fr[k++] = 15; memcpy(fr + k, "veryCoolNetwork", 15); k += 15;
		fr[k++] = 1; fr[k++] = 8; memcpy(fr + k, "\x8c\x12\x98\x24\xb0\x48\x60\x6c", 8); k += 8;
		memcpy(fr + k, sim_ht_cap, 28); k += 28; memcpy(fr + k, sim_ht_op, 24); k += 24; memcpy(fr + k, sim_wmm, 26); k += 26;
		a34w_mgmt_in(fr, k, -74, 48);
		a34w_joining = false;
		a34w_net.psk_given = false;
		*a34w_mbox_word(4) = *a34w_mbox_word(0);
		nsent = 0; sim_on = 1; sim_ap = 1; sim_ap_ht = 1; memcpy(sim_ap_addr, ap, 6);
		a34w_joined = false;
		bool joined = a34w_connect();
		firmware_sim();
		sim_on = 0; sim_ap = 0; sim_ap_ht = 0;
		for (uint32_t i = 0; i < nsent; i++) {
			bool cmd = ((dw_of(sent[i], 1) >> 16) & 3u) == 1u;
			if (!cmd && (sent[i][32] >> 4) == 0) assoc_at = (int)i;
			if (cmd && sent[i][36] == 0x1d) wmm_at = (int)i;
			if (cmd && sent[i][36] == 0x12 && bss_at < 0) bss_at = (int)i;
			if (cmd && sent[i][36] == 0x13 && sent[i][64 + 20] == 2) sta3_at = (int)i;
		}
		printf("0968 HT join: %s; Association Request at %d, WMM parameters at %d, BSS info at %d, STA record (state 3) at %d\n",
		    joined ? "joined" : "NOT JOINED", assoc_at, wmm_at, bss_at, sta3_at);
		failed |= !joined || assoc_at < 0 || wmm_at < 0 || bss_at < 0 || sta3_at < 0 || wmm_at > bss_at;
		if (assoc_at >= 0) {
			const uint8_t *f = sent[assoc_at] + 32;
			n = sent_len[assoc_at] - 32;
			bzero(ies, sizeof(ies));
			memcpy(ies, f + 28, n - 28 < sizeof(ies) ? n - 28 : sizeof(ies));
			failed |= a34w_ie_in(ies, n - 28, 45, -1) == NULL || a34w_ie_in(ies, n - 28, 127, -1) == NULL ||
			    a34w_ie_in(ies, n - 28, 221, 0) == NULL || a34w_ie_in(ies, n - 28, 45, -1)[2] != 0x2d;
		}
		if (sta3_at >= 0) {
			const uint8_t *c = sent[sta3_at] + 64;
			printf("  STA_REC: QoS %u, phy 0x%02x, MCS 0x%02x 0x%02x, HT cap info 0x%04x, A-MPDU 0x%02x, TX/RX A-MPDU %u/%u, BA %u/%u, RTS %u\n",
			    c[18], c[13], c[24], c[25], c[40] | c[41] << 8, c[48], c[76], c[77], c[92], c[93], c[68]);
			failed |= c[18] != 1 || c[13] != 0x18 || c[24] != 0xff || c[25] != 0xff || (c[40] | c[41] << 8) != (0x09ef & ~0x42) ||
			    c[48] != 0x17 || c[76] != 1 || c[77] != 1 || c[92] != 64 || c[68] != 3;
		}
		if (bss_at >= 0) {
			const uint8_t *c = sent[bss_at] + 64;
			printf("  BSS_INFO: QBSS %u, phy 0x%02x, RLM HT info1 0x%02x info2 0x%02x protect %u\n", c[42], c[55], c[68 + 12], c[68 + 10],
			    c[68 + 5]);
			failed |= c[42] != 1 || c[55] != 0x18 || c[68 + 12] != 0x00 || c[68 + 10] != 0x04;
		}
		if (wmm_at >= 0) {
			const uint8_t *c = sent[wmm_at] + 64;
			printf("  WMM: BE CW %u-%u AIFSN %u, VO CW %u-%u AIFSN %u TXOP %u\n", c[0] | c[1] << 8, c[2] | c[3] << 8, c[6],
			    c[30] | c[31] << 8, c[32] | c[33] << 8, c[36], c[34] | c[35] << 8);
			failed |= (c[0] | c[1] << 8) != 15 || (c[2] | c[3] << 8) != 1023 || c[6] != 3 || (c[30] | c[31] << 8) != 3 ||
			    (c[32] | c[33] << 8) != 7 || c[36] != 2 || (c[34] | c[35] << 8) != 0x2f || c[41] != 1;
		}
		/* a BSS transition request with an MBO element: answered with status 7 and an MBO reason */
		bzero(fr, sizeof(fr));
		fr[0] = 0xd0; memcpy(fr + 4, a34w_mac, 6); memcpy(fr + 10, ap, 6); memcpy(fr + 16, ap, 6);
		fr[24] = 10; fr[25] = 7; fr[26] = 1; fr[27] = 0x01; fr[28] = 0; fr[29] = 0; fr[30] = 200;
		memcpy(fr + 31, "\xdd\x07\x50\x6f\x9a\x16\x01\x01\x00", 9);
		*a34w_mbox_word(4) = *a34w_mbox_word(0);
		nsent = 0; sim_on = 1;
		a34w_mgmt_in(fr, 40, -74, 48);
		firmware_sim();
		sim_on = 0;
		printf("  BTM: %u frames; category %u action %u token %u status %u, %u bytes\n", nsent, sent[0][32 + 24], sent[0][32 + 25],
		    sent[0][32 + 26], sent[0][32 + 27], sent_len[0] - 32);
		failed |= nsent != 1 || sent[0][32 + 24] != 10 || sent[0][32 + 25] != 8 || sent[0][32 + 26] != 1 || sent[0][32 + 27] != 7 ||
		    sent_len[0] - 32 != 24 + 5 + 9 || sent[0][32 + 29] != 221;
		printf("0968 HT and WMM station, BSS transition answered: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}

	/* ---- 0969: iOS's Wi-Fi side: the interface in the registry, Apple80211 requests as wifid makes them ---- */
	{
		static const uint8_t ap[6] = { 0x02, 0x51, 0x4d, 0x00, 0x00, 0x01 };
		struct apple80211req rq;
		static struct apple80211_scan_request scan;
		static struct apple80211_scan_result result;
		static struct apple80211_assoc assoc;
		uint8_t caps[13];
		struct { uint32_t version, radios, state[4]; } power;
		int failed = 0, e;

		a34w_joined = false; a34w_attached = false; a34w_done = 9; a34w_sta_off = false;
		/* the domain on and reachable, the firmware's ready bits: what a scan checks first */
		a34w_wr(a34w_ap, SPM_PWR_STATUS, CONN_PWR_STA); a34w_wr(a34w_ap, SPM_PWR_STATUS_2ND, CONN_PWR_STA);
		a34w_wr(a34w_ap, SPM_CONN_PWR_CON, PWR_RST_B); a34w_wr(a34w_ap, INFRA_PROTECTEN_STA1, 0);
		a34w_wr(a34w_conn, HOST_SW_SYNC0, 3);
		failed |= !a34w_attach() || strcmp(wlan_registered, "en9") != 0;
		/* CARD_CAPABILITIES (WPA2 bit 14), POWER */
		bzero(&rq, sizeof(rq)); rq.req_type = 12; rq.req_len = sizeof(caps); rq.req_data = (user_addr_t)(uintptr_t)caps;
		e = a34_apple80211_ioctl(SIOCGA80211, &rq);
		failed |= e != 0 || (caps[1] & 0x40u) == 0;
		bzero(&rq, sizeof(rq)); rq.req_type = 19; rq.req_len = sizeof(power); rq.req_data = (user_addr_t)(uintptr_t)&power;
		e = a34_apple80211_ioctl(SIOCGA80211, &rq);
		failed |= e != 0 || power.state[0] != 1;
		/* a scan: asked, done by the thread (here inline), its results one by one, then EIO */
		*a34w_mbox_word(4) = *a34w_mbox_word(0);
		sim_on = 1; sim_ap = 1; memcpy(sim_ap_addr, ap, 6); kev_count = 0;
		bzero(&scan, sizeof(scan)); scan.scan_type = 1;
		bzero(&rq, sizeof(rq)); rq.req_type = 10; rq.req_len = sizeof(scan); rq.req_data = (user_addr_t)(uintptr_t)&scan;
		e = a34_apple80211_ioctl(SIOCSA80211, &rq);
		failed |= e != 0 || a34w_sta_scan != 1;
		a34w_sta_scan = 0;
		a34w_sta_do(false, NULL, true);
		bzero(&result, sizeof(result)); result.elements_length = 1024;
		bzero(&rq, sizeof(rq)); rq.req_type = 11; rq.req_len = sizeof(result); rq.req_data = (user_addr_t)(uintptr_t)&result;
		e = a34_apple80211_ioctl(SIOCGA80211, &rq);
		printf("0969 scan: SCAN_DONE posted %d; first result: %d, \"%.*s\" ch %u %d dBm, %u elements bytes, %u rates\n",
		    kev_count > 0 && kev_codes[kev_count - 1] == 10, e, result.ssid_length, result.ssid, result.channel.channel, result.rssi,
		    result.elements_length, result.rate_count);
		failed |= e != 0 || result.ssid_length != 9 || memcmp(result.ssid, "Sim Open", 8) != 0 || result.channel.channel != 6 ||
		    result.rate_count != 4 || kev_count == 0 || kev_codes[kev_count - 1] != 10;
		e = a34_apple80211_ioctl(SIOCGA80211, &rq);
		failed |= e != EIO;
		/* a join from Settings: the name and nothing else (open), done by the thread, ASSOCIATE_RESULT 1, STATE 4, SSID */
		bzero(&assoc, sizeof(assoc)); assoc.ssid_length = 9; memcpy(assoc.ssid, "Sim Open", 9);
		bzero(&rq, sizeof(rq)); rq.req_type = 20; rq.req_len = sizeof(assoc); rq.req_data = (user_addr_t)(uintptr_t)&assoc;
		e = a34_apple80211_ioctl(SIOCSA80211, &rq);
		failed |= e != 0 || a34w_sta_join_asked != 1;
		{
			struct a34_sta_join what = a34w_sta_join_what;
			a34w_sta_join_asked = 0;
			kev_count = 0;
			a34w_sta_do(false, &what, false);
		}
		firmware_sim();
		sim_on = 0; sim_ap = 0;
		bzero(&rq, sizeof(rq)); rq.req_type = 21;
		(void)a34_apple80211_ioctl(SIOCGA80211, &rq);
		{
			int32_t result21 = rq.req_val;
			char name[33] = "";
			bzero(&rq, sizeof(rq)); rq.req_type = 13;
			(void)a34_apple80211_ioctl(SIOCGA80211, &rq);
			printf("0969 join: joined %d, ASSOCIATE_RESULT %d, STATE %d, events:", a34w_joined, result21, rq.req_val);
			for (uint32_t i = 0; i < kev_count; i++) printf(" %u", kev_codes[i]);
			failed |= !a34w_joined || result21 != 1 || rq.req_val != 4;
			bzero(&rq, sizeof(rq)); rq.req_type = 1; rq.req_len = 32; rq.req_data = (user_addr_t)(uintptr_t)name;
			e = a34_apple80211_ioctl(SIOCGA80211, &rq);
			printf("; SSID \"%s\" (%u bytes)\n", name, rq.req_len);
			failed |= e != 0 || strcmp(name, "Sim Open") != 0;
		}
		/* the link events: SSID (2), BSSID (3), LINK (4) changed, then ASSOC_DONE (9) */
		failed |= kev_count < 4 || kev_codes[kev_count - 1] != 9;
		printf("0969 iOS's Wi-Fi side (registry, Apple80211 scan and join): %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}

	/* ---- 1010: every token in flight, one report waiting on ring 4: the frame waits, takes it back, goes ---- */
	{
		uint8_t f[64], e[60];
		int failed = 0;
		uint64_t drops0 = a34w_data_dropped_out;
		bool sent;

		a34w_joined = true;
		memset(a34w_tokens_busy, 0xff, sizeof(a34w_tokens_busy));
		bzero(f, sizeof(f));
		put32(f, (6u << 27) | (1u << 16) | 12u); put32(f + 4, 3u << 16); put32(f + 8, (77u << 16) | 1u);   /* token 77 back */
		rx_put(4, f, 12);
		bzero(e, sizeof(e)); memcpy(e, a34w_target.bssid, 6); memcpy(e + 6, a34w_mac, 6); e[12] = 0x08; e[14] = 0x45;
		sent = a34w_data_tx(e, sizeof(e), false, true, false);
		printf("1010 TX with every token busy: %s, token 77 %s, %llu waited, dropped %llu\n", sent ? "sent" : "NOT SENT",
		    (a34w_tokens_busy[1] >> 13) & 1u ? "taken again" : "free", (unsigned long long)a34w_tx_waits,
		    (unsigned long long)(a34w_data_dropped_out - drops0));
		failed |= !sent || ((a34w_tokens_busy[1] >> 13) & 1u) == 0 || a34w_data_dropped_out != drops0;
		/* and with none coming back: dropped after 5 ms, counted as "no token" */
		sent = a34w_data_tx(e, sizeof(e), false, true, false);
		failed |= sent || a34w_drop_no_token != 1;
		bzero(a34w_tokens_busy, sizeof(a34w_tokens_busy));
		printf("1010 TX waits for tokens: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}

	/* ---- 1010: a scan for a name (hidden networks), and Settings' Wi-Fi switch ---- */
	{
		static struct apple80211_scan_request scan;
		struct apple80211req rq;
		struct { uint32_t version, radios, state[4]; } power;
		int failed = 0, e;
		uint32_t at = 0;

		*a34w_mbox_word(4) = *a34w_mbox_word(0);
		nsent = 0; sim_on = 1; sim_ap = 1;
		bzero(&scan, sizeof(scan)); scan.scan_type = 1; scan.ssid_length = 6; memcpy(scan.ssid, "Hidden", 6);
		bzero(&rq, sizeof(rq)); rq.req_type = 10; rq.req_len = sizeof(scan); rq.req_data = (user_addr_t)(uintptr_t)&scan;
		e = a34_apple80211_ioctl(SIOCSA80211, &rq);
		failed |= e != 0 || a34w_sta_scan != 1 || a34w_sta_scan_ssid_length != 6;
		a34w_sta_scan = 0;
		a34w_sta_do(false, NULL, true);
		firmware_sim();
		for (uint32_t i = 0; i < nsent; i++) if (sent[i][36] == 0x03) at = i;
		printf("1010 named scan: type %u, SSID type %u, %u name(s): \"%.*s\"\n", sent[at][64 + 2], sent[at][64 + 3],
		    sent[at][64 + 4], sent[at][64 + 8], (const char *)sent[at] + 64 + 12);
		failed |= sent[at][64 + 2] != 1 || sent[at][64 + 3] != 4 || sent[at][64 + 4] != 1 || sent[at][64 + 8] != 6 ||
		    memcmp(sent[at] + 64 + 12, "Hidden", 6) != 0;
		/* the switch off: the network left, no rejoin, scans refused; on: told, scans again */
		a34w_joined = true;
		kev_count = 0;
		power.version = 1; power.radios = 1; power.state[0] = 0;
		bzero(&rq, sizeof(rq)); rq.req_type = 19; rq.req_len = sizeof(power); rq.req_data = (user_addr_t)(uintptr_t)&power;
		e = a34_apple80211_ioctl(SIOCSA80211, &rq);
		failed |= e != 0 || a34w_sta_leave != 1 || !a34_sta_switched_off();
		a34w_sta_leave = 0;
		a34w_sta_do(true, NULL, false);
		failed |= a34w_joined || a34w_auto_rejoin_at != 0;
		bzero(&rq, sizeof(rq)); rq.req_type = 10; rq.req_len = sizeof(scan); rq.req_data = (user_addr_t)(uintptr_t)&scan;
		failed |= a34_apple80211_ioctl(SIOCSA80211, &rq) != ENETDOWN;
		power.state[0] = 1;
		bzero(&rq, sizeof(rq)); rq.req_type = 19; rq.req_len = sizeof(power); rq.req_data = (user_addr_t)(uintptr_t)&power;
		e = a34_apple80211_ioctl(SIOCSA80211, &rq);
		failed |= e != 0 || a34_sta_switched_off() || kev_count == 0 || kev_codes[kev_count - 1] != 1;
		bzero(&rq, sizeof(rq)); rq.req_type = 10; rq.req_len = sizeof(scan); rq.req_data = (user_addr_t)(uintptr_t)&scan;
		failed |= a34_apple80211_ioctl(SIOCSA80211, &rq) != 0;
		a34w_sta_scan = 0;
		sim_on = 0; sim_ap = 0;
		printf("1010 named scan, the Wi-Fi switch off and on: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}

	/* ---- 1010: BSS transition: taken when the AP says it will let us go, refused for one we never heard ---- */
	{
		uint8_t f[96];
		static const uint8_t other[6] = { 0xc4, 0x9a, 0x31, 0x2a, 0xc8, 0x59 };
		int failed = 0;

		a34w_joined = true; a34w_roam_pending = false; a34w_link_rssi = -80;
		for (int imminent = 1; imminent >= 0; imminent--) {
			bzero(f, sizeof(f));
			f[0] = 0xd0; memcpy(f + 4, a34w_mac, 6); memcpy(f + 10, a34w_target.bssid, 6); memcpy(f + 16, a34w_target.bssid, 6);
			f[24] = 10; f[25] = 7; f[26] = 9; f[27] = (uint8_t)(0x01 | (imminent ? 0x04 : 0)); f[28] = 10; f[30] = 100;
			f[31] = 52; f[32] = 16; memcpy(f + 33, other, 6); f[43] = 81; f[44] = 5; f[45] = 7;   /* neighbor report: channel 5 */
			f[46] = 3; f[47] = 1; f[48] = 200;                                                         /* preference 200 */
			*a34w_mbox_word(4) = *a34w_mbox_word(0);
			nsent = 0; sim_on = 1;
			a34w_mgmt_in(f, 49, -60, 48);
			firmware_sim();
			sim_on = 0;
			printf("1010 BSS transition (%s): status %u, %u bytes%s, move %s\n", imminent ? "disassociation imminent" : "a candidate never heard",
			    sent[0][32 + 27], sent_len[0] - 32, sent_len[0] - 32 == 35 && memcmp(sent[0] + 32 + 29, other, 6) == 0 ? " with the target" : "",
			    a34w_roam_pending ? "scheduled" : "not");
			if (imminent) {
				failed |= nsent != 1 || sent[0][32 + 27] != 0 || memcmp(sent[0] + 32 + 29, other, 6) != 0 || !a34w_roam_pending ||
				    memcmp(a34w_roam_to, other, 6) != 0;
			} else {
				failed |= nsent != 1 || sent[0][32 + 27] != 7 || a34w_roam_pending;
			}
			a34w_roam_pending = false;
		}
		printf("1010 BSS transition taken or refused: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}

	/* ---- 0966: PHY_ACTION out of the vendor's default NVRAM image (FW_DIR/WIFI), as wlanPreCal builds it ---- */
	{
		static const uint8_t head[] = { 0xaa, 0x89, 0x67, 0x55, 0x03, 0x01, 0x7c, 0x01, 0x01, 0x00, 0x04, 0x00, 0x01, 0x00,
			                        0x00, 0x00, 0x00, 0x00, 0x04, 0x01, 0x07 };
		uint32_t slot = a34w_tx_cidx, size = 0;
		const uint8_t *f = (const uint8_t *)a34w_dma + DMA_TX17_BUFS + slot * SLOT_SIZE, *pay = f + 64;
		uint8_t *image = a34w_file("WIFI", 512u, "-", &size);
		int failed = image == NULL;

		(void)a34w_phy_action();                /* (no answer here: the frame is what is checked) */
		if (image != NULL) {
			uint32_t length = (a34w_le32(f) & 0xffffu) - 64u, at = 256, start = 0;
			while (at + 3 <= size && image[at] != 10) {          /* tag 10's offset: the NVRAM part ends there */
				if (image[at] == 7) start = at;
				at += 3u + (uint32_t)(image[at + 1] | image[at + 2] << 8);
			}
			printf("0966 PHY_ACTION: %u bytes, command 0x%02x, NVRAM tags 7-9 %u bytes (file %u..%u)\n", length, f[36],
			    pay[18] | pay[19] << 8, start, at);
			failed |= length != 388 || f[36] != 8 || memcmp(pay, head, sizeof(head)) != 0 || (pay[18] | pay[19] << 8) != 260 ||
			    memcmp(pay + 20, image + start, at - start) != 0 || pay[280] != 2 || pay[282] != 104;
			for (uint32_t i = 284; i < 388; i++) failed |= pay[i] != 0;
			kfree_data(image, size);
		}
		printf("0966 PHY_ACTION frame: %s\n", failed ? "WRONG" : "ok");
		bad |= failed;
	}
	printf("log:\n%s", a34w_log);
	return bad;
}
