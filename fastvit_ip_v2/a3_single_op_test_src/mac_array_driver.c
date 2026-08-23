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

int mac_wait_done_timeout(int timeout_ms) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (REG_RD(mac_ctrl, MAC_AP_CTRL_OFFSET) & MAC_AP_DONE) return 0;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (elapsed_ms > timeout_ms) return 1;
        usleep(1000);
    }
}

static void w64(uint32_t lo_off, uint32_t hi_off, uintptr_t phys_addr) {
    REG_WR(mac_ctrl, lo_off, (uint32_t)(phys_addr));
    REG_WR(mac_ctrl, hi_off, (uint32_t)((uint64_t)phys_addr >> 32));
}

void mac_run_layers(
    uintptr_t desc_phys, int n_layers,
    uintptr_t in_base_phys,  size_t in_flush_size,
    uintptr_t w_base_phys,   size_t w_flush_size,
    uintptr_t b_base_phys,   size_t b_flush_size,
    uintptr_t out_base_phys,
    uintptr_t out_written_phys,
    uintptr_t out_check_off_phys, size_t out_check_size)
{
    /* CPU -> FPGA: every buffer the IP's own m_axi masters will read,
     * including desc[] itself (gmem_meta) -- the one handoff the old
     * architecture never had, per mac_array_driver.h's header note. */
    mac_cache_flush(desc_phys, (size_t)n_layers * sizeof(MacLayerDesc));
    mac_cache_flush(in_base_phys, in_flush_size);
    mac_cache_flush(w_base_phys,  w_flush_size);
    mac_cache_flush(b_base_phys,  b_flush_size);
    w64(MAC_DESC_LO, MAC_DESC_HI, desc_phys);
    REG_WR(mac_ctrl, MAC_N_LAYERS, (uint32_t)n_layers);
    w64(MAC_IN_BASE_LO, MAC_IN_BASE_HI, in_base_phys);
    w64(MAC_W_BASE_LO, MAC_W_BASE_HI, w_base_phys);
    w64(MAC_B_BASE_LO, MAC_B_BASE_HI, b_base_phys);
    w64(MAC_OUT_BASE_LO, MAC_OUT_BASE_HI, out_base_phys);
    w64(MAC_OUT_WRITTEN_LO, MAC_OUT_WRITTEN_HI, out_written_phys);
    /* A3 MERGE round (2026-08-23, ZHR-92): in_base_wide -- always the same
     * physical region as in_base (harmless to set even for op types, e.g.
     * Add, that never read it). */
    w64(MAC_IN_BASE_WIDE_LO, MAC_IN_BASE_WIDE_HI, in_base_phys);

    REG_WR(mac_ctrl, MAC_AP_CTRL_OFFSET, MAC_AP_START);
    mac_wait_done();

    /* FPGA -> CPU: invalidate before the ARM trusts anything the IP wrote. */
    mac_cache_invalidate(out_check_off_phys, out_check_size);
    mac_cache_invalidate(out_written_phys, (size_t)n_layers * sizeof(int32_t));
}

int mac_check_written(const int32_t *out_written_virt, int n_layers) {
    for (int i = 0; i < n_layers; i++)
        if (out_written_virt[i] == 0) return 0;
    return 1;
}
