/*================================================================
 * mac_array_driver.c -- A3 driver implementation, see mac_array_driver.h.
 *================================================================*/
#include "mac_array_driver.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

static int fd_mem = -1;
static volatile void *mac_ctrl = NULL;

#define REG_WR(vbase, off, val) \
    (*(volatile uint32_t*)((char*)(vbase) + (off)) = (uint32_t)(val))
#define REG_RD(vbase, off) \
    (*(volatile uint32_t*)((char*)(vbase) + (off)))

int mac_driver_init(void) {
    fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_mem < 0) { perror("open /dev/mem"); return -1; }
    mac_ctrl = mmap(NULL, MAC_ARRAY_MAP_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd_mem, MAC_ARRAY_CTRL_PHYS);
    if (mac_ctrl == MAP_FAILED) {
        perror("mmap mac_array_top s_axi_control");
        mac_ctrl = NULL;
        return -1;
    }
    return 0;
}

void mac_driver_exit(void) {
    if (mac_ctrl) munmap((void*)mac_ctrl, MAC_ARRAY_MAP_SIZE);
    if (fd_mem >= 0) close(fd_mem);
}

void mac_cache_flush(uintptr_t phys_addr, size_t size) {
    __builtin___clear_cache((char*)phys_addr, (char*)(phys_addr + size));
}

void mac_cache_invalidate(uintptr_t phys_addr, size_t size) {
    __builtin___clear_cache((char*)phys_addr, (char*)(phys_addr + size));
}

void mac_wait_done(void) {
    while (!(REG_RD(mac_ctrl, MAC_AP_CTRL_OFFSET) & MAC_AP_DONE))
        ;
}

/* ZHR-92 (2026-09-15) BUSY-POLL: this loop used to end in usleep(1000),
 * which on this board's kernel sleeps ~1.075ms (measured: usleep(0) is
 * already 0.074ms -- the floor is scheduler granularity, so no usleep()
 * value can reach a 0.1ms resolution). Every per-entry time the
 * full-network harness ever reported was therefore a multiple of ~1.08ms,
 * i.e. +-50k cycles per entry -- fine for totals, useless for per-layer
 * fits once DW layers dropped to 2-4ms. Busy-polling costs ~1.3us per
 * iteration (AP_CTRL read 0.14us + clock_gettime 1.1us, both measured on
 * the board) and the ARM has nothing else to do while the PL runs (the
 * dispatch is strictly serial), so the CPU cost is free. The timeout is
 * still checked on every iteration. MAC_WAIT_SLEEP_US restores a sleeping
 * poll for anyone who needs the old behaviour. */
int mac_wait_done_timeout(int timeout_ms) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (REG_RD(mac_ctrl, MAC_AP_CTRL_OFFSET) & MAC_AP_DONE) return 0;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (elapsed_ms > timeout_ms) return 1;
#ifdef MAC_WAIT_SLEEP_US
        usleep(MAC_WAIT_SLEEP_US);
#endif
    }
}

static void w64(uint32_t lo_off, uint32_t hi_off, uintptr_t phys_addr) {
    REG_WR(mac_ctrl, lo_off, (uint32_t)(phys_addr));
    REG_WR(mac_ctrl, hi_off, (uint32_t)((uint64_t)phys_addr >> 32));
}

/* ZHR-92 (2026-08-30): writes desc's 28 fields directly to the
 * MAC_DESC_BASE register block, one 32-bit register per field, in
 * MacLayerDesc's own declaration order -- mirrors exactly how HLS
 * flattened the struct-by-value s_axilite port (confirmed empirically via
 * a standalone probe before this was written, see ZHR-92's recon comment:
 * field k lands at MAC_DESC_BASE+4*k, no padding, since every field is
 * int32_t). Treating the struct as a raw int32_t array relies on the
 * _Static_assert in mac_array_driver.h (sizeof(MacLayerDesc) ==
 * MAC_DESC_NUM_FIELDS*4) to catch any future field-count drift at compile
 * time instead of silently writing the wrong number of registers. */
void mac_write_desc(const MacLayerDesc *desc) {
    const int32_t *fields = (const int32_t *)desc;
    for (int k = 0; k < MAC_DESC_NUM_FIELDS; k++) {
        REG_WR(mac_ctrl, MAC_DESC_BASE + 4 * k, (uint32_t)fields[k]);
    }
}

int mac_run_layers(
    const MacLayerDesc *desc,
    uintptr_t in_base_phys,  size_t in_flush_size,
    uintptr_t w_base_phys,   size_t w_flush_size,
    uintptr_t b_base_phys,   size_t b_flush_size,
    uintptr_t out_base_phys,
    uintptr_t out_check_off_phys, size_t out_check_size)
{
    /* CPU -> FPGA: every buffer the IP's own m_axi masters will read.
     * desc itself no longer needs a flush -- it's written straight into
     * s_axi_control registers below, never touches DRAM at all (the
     * gmem_meta master this used to require is gone). */
    mac_cache_flush(in_base_phys, in_flush_size);
    mac_cache_flush(w_base_phys,  w_flush_size);
    mac_cache_flush(b_base_phys,  b_flush_size);
    mac_write_desc(desc);
    w64(MAC_IN_BASE_LO, MAC_IN_BASE_HI, in_base_phys);
    w64(MAC_W_BASE_LO, MAC_W_BASE_HI, w_base_phys);
    w64(MAC_B_BASE_LO, MAC_B_BASE_HI, b_base_phys);
    w64(MAC_OUT_BASE_LO, MAC_OUT_BASE_HI, out_base_phys);
    /* A3 MERGE round (2026-08-23, ZHR-92): in_base_wide -- always the same
     * physical region as in_base (harmless to set even for op types, e.g.
     * Add, that never read it). */
    w64(MAC_IN_BASE_WIDE_LO, MAC_IN_BASE_WIDE_HI, in_base_phys);
    /* A3 row-hoist round (2026-08-25, ZHR-92): in_burst -- same physical
     * region as in_base/in_base_wide (see MAC_IN_BURST_LO/HI's own comment
     * in the driver header). Flagged and fixed before board deployment
     * this time, not after (out_burst's own equivalent gap above was only
     * caught post-board). */
    w64(MAC_IN_BURST_LO, MAC_IN_BURST_HI, in_base_phys);
    /* ZHR-92 (2026-08-25): out_burst -- same physical region as out_base
     * (see MAC_OUT_BURST_LO/HI's own comment in the driver header).
     * Found missing here during the full-network-hang investigation's
     * "check every dispatch site sets it" pass -- this function currently
     * has no callers among the test harnesses (all of them deliberately
     * bypass it for mac_wait_done()'s unbounded wait, see
     * mac_array_single_op_test.c's own header comment), so this was not
     * an active risk for anything tested so far, but it's a real gap for
     * whoever calls this function next. */
    w64(MAC_OUT_BURST_LO, MAC_OUT_BURST_HI, out_base_phys);

    REG_WR(mac_ctrl, MAC_AP_CTRL_OFFSET, MAC_AP_START);
    /* ZHR-92 (2026-08-25): this wait is UNBOUNDED -- no timeout, no
     * external kill path. Every other dispatch site in this project's
     * test harnesses (mac_array_single_op_test.c, _add.c,
     * mac_array_full_network_test.c) now uses a bounded 30s wait after
     * this same investigation found the opposite (an unbounded/
     * effectively-unbounded wait) directly implicated in a real board
     * hang that took a full recovery cycle to clear. This function's
     * design predates that finding and was NOT changed here -- switching
     * it to a bounded wait is a real API/behavior decision (some callers
     * may deliberately want a blocking wait), not a mechanical fix, and
     * is left to a deliberate choice rather than done silently. Anyone
     * adding a new caller of mac_run_layers should not assume "trusted
     * architecture" makes an unbounded wait safe -- that assumption is
     * exactly what this investigation could not fully confirm or rule
     * out (see ZHR-92's hang-reproduction rounds). */
    mac_wait_done();

    /* FPGA -> CPU: invalidate before the ARM trusts anything the IP wrote. */
    mac_cache_invalidate(out_check_off_phys, out_check_size);
    /* out_written is a plain s_axi_control register now, not a DRAM
     * pointer -- no cache invalidate needed for it at all (mmap'd
     * register space is never CPU-cached the way DRAM is). Read directly;
     * ap_done already implies the IP's own write to this register
     * happened, same guarantee `return` values rely on generally. */
    return (int)REG_RD(mac_ctrl, MAC_OUT_WRITTEN_DATA);
}
