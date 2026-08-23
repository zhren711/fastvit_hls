/*================================================================
 * mac_array_single_op_test.c -- A3 board bring-up STEP 1 (ZHR-92,
 * 2026-08-21): the first-ever real hardware dispatch on the new
 * layer-controller + MAC array architecture. Dispatches ONE PW conv
 * (entry[3] of the real 82-entry hardware sequence, relocated into a
 * compact standalone buffer by tools/build_single_op_test_entry3.py) and
 * checks byte-exact agreement against the real csim reference for that
 * same entry -- not "did it run", but "did it compute the right answer."
 *
 * Deliberately does NOT use mac_run_layers()'s unbounded mac_wait_done()
 * -- ZHR-10's precedent (an "HLS/Vivado all-green" design that hung the
 * real board) means the first-ever dispatch on this architecture must be
 * bounded, so a hang shows up as a clean timeout instead of a dead SSH
 * session. Also reads back a known register BEFORE dispatch (bitstream
 * loaded? clock enabled? base address right? -- ZHR-5's three cheap,
 * frequently-guilty checks) rather than assuming a clean csynth/P&R
 * history means the real fabric is configured correctly.
 *
 * 用法: mac_array_single_op_test <bundle_dir containing desc.bin, in.bin,
 *       w.bin, b.bin, ref_out.bin -- see build_single_op_test_entry3.py>
 *================================================================*/
#include "mac_array_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#define FV_DDR_BASE   0x10000000UL
/* A3 round (2026-08-23, ZHR-92): widened 1MB->3MB and the region offsets
 * spread out accordingly -- the original 1MB layout's IN/OUT regions were
 * only ~0xC0000 (786,432 bytes) apart from the NEXT region, which fit
 * every single-op bundle tested so far (entry3/entry5_dw/entry10_add, all
 * <=196,608 bytes) but segfaulted (SIGSEGV, exit 139) the first time a
 * bundle needed the full 786,432 bytes both in AND out (entry[0]'s real
 * GELU shape, cin=48/h=w=128) -- OUT_OFF+ref_size overran W_OFF/B_OFF/
 * OUT_WRITTEN_OFF, all of which sat inside what should have been the IN
 * region's own space, and OUT's own write extended past the 1MB mmap
 * window entirely. Purely additive: same relative DESC/IN/W/B/OUT/
 * OUT_WRITTEN structure, just spaced far enough apart (0x100000 = 1MB
 * per region) to hold anything up to ~1MB, which comfortably covers
 * every real single-op shape in the network (the largest, entry[0]'s
 * GELU, is 786,432 bytes). */
#define FV_MAP_SIZE   0x300000UL   /* 3MB window */

#define DESC_OFF          0x000000UL
#define IN_OFF            0x010000UL
#define W_OFF             0x110000UL
#define B_OFF             0x120000UL
#define OUT_OFF           0x130000UL
#define OUT_WRITTEN_OFF   0x230000UL

static size_t file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return (size_t)sz;
}

static void load_file(const char *path, void *dst, size_t expect_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    size_t n = fread(dst, 1, expect_size, f);
    fclose(f);
    if (n != expect_size) {
        fprintf(stderr, "short read %s: got %zu, want %zu\n", path, n, expect_size);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <bundle_dir>\n", argv[0]);
        return 1;
    }
    char path[512];
    const char *dir = argv[1];

    snprintf(path, sizeof(path), "%s/desc.bin", dir);
    size_t desc_size = file_size(path);
    snprintf(path, sizeof(path), "%s/in.bin", dir);
    size_t in_size = file_size(path);
    snprintf(path, sizeof(path), "%s/w.bin", dir);
    size_t w_size = file_size(path);
    snprintf(path, sizeof(path), "%s/b.bin", dir);
    size_t b_size = file_size(path);
    snprintf(path, sizeof(path), "%s/ref_out.bin", dir);
    size_t ref_size = file_size(path);

    printf(">>> bundle sizes: desc=%zu in=%zu w=%zu b=%zu ref_out=%zu\n",
           desc_size, in_size, w_size, b_size, ref_size);

    /* ---- map DRAM scratch region (same FV_DDR_BASE convention as the
     * old-architecture petalinux driver -- proven safe on this board
     * across this project's whole board-bring-up history, reused as-is,
     * not redesigned). ---- */
    int fd_dma = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_dma < 0) { perror("open /dev/mem (dma)"); return 1; }
    void *dma_virt = mmap(NULL, FV_MAP_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd_dma, FV_DDR_BASE);
    if (dma_virt == MAP_FAILED) { perror("mmap dma region"); return 1; }

    uint8_t *desc_v = (uint8_t*)dma_virt + DESC_OFF;
    uint8_t *in_v   = (uint8_t*)dma_virt + IN_OFF;
    uint8_t *w_v    = (uint8_t*)dma_virt + W_OFF;
    uint8_t *b_v    = (uint8_t*)dma_virt + B_OFF;
    uint8_t *out_v  = (uint8_t*)dma_virt + OUT_OFF;
    int32_t *out_written_v = (int32_t*)((uint8_t*)dma_virt + OUT_WRITTEN_OFF);

    snprintf(path, sizeof(path), "%s/desc.bin", dir);
    load_file(path, desc_v, desc_size);
    snprintf(path, sizeof(path), "%s/in.bin", dir);
    load_file(path, in_v, in_size);
    snprintf(path, sizeof(path), "%s/w.bin", dir);
    load_file(path, w_v, w_size);
    snprintf(path, sizeof(path), "%s/b.bin", dir);
    load_file(path, b_v, b_size);

    /* Poison the output region BEFORE dispatch so "output changed" is a
     * real check, not a coincidence of a zero-filled page -- 0xA5 is not
     * a plausible int8 conv output pattern (every byte identical). */
    memset(out_v, 0xA5, ref_size);
    out_written_v[0] = 0;
    /* Flush the poison pattern + written=0 too -- otherwise a stale cache
     * line could make the post-dispatch invalidate look like a false
     * pass by accident. */
    mac_cache_flush(FV_DDR_BASE + OUT_OFF, ref_size);
    mac_cache_flush(FV_DDR_BASE + OUT_WRITTEN_OFF, sizeof(int32_t));

    /* ---- map + probe the IP's control register BEFORE dispatch (ZHR-5:
     * bitstream loaded? clock enabled? base address right? -- cheaper to
     * check than to assume from a clean synthesis/P&R history). ---- */
    if (mac_driver_init() != 0) {
        fprintf(stderr, "mac_driver_init failed\n");
        return 1;
    }
    /* mac_driver_init() doesn't expose mac_ctrl directly -- re-mmap here
     * just for the raw pre-flight readback/printout (harmless, same
     * physical window, PROT_READ is enough for this one probe). */
    int fd_ctrl = open("/dev/mem", O_RDONLY | O_SYNC);
    void *ctrl_probe = mmap(NULL, MAC_ARRAY_MAP_SIZE, PROT_READ, MAP_SHARED,
                             fd_ctrl, MAC_ARRAY_CTRL_PHYS);
    if (ctrl_probe == MAP_FAILED) { perror("mmap ctrl probe"); return 1; }
    uint32_t ap_ctrl_before = *(volatile uint32_t*)((char*)ctrl_probe + MAC_AP_CTRL_OFFSET);
    munmap(ctrl_probe, MAC_ARRAY_MAP_SIZE);
    close(fd_ctrl);
    printf(">>> PRE-FLIGHT: AP_CTRL @ 0x%08lx = 0x%08x", MAC_ARRAY_CTRL_PHYS, ap_ctrl_before);
    if (ap_ctrl_before == 0xFFFFFFFFu)
        printf("  <-- ALL-F: bitstream not loaded / bus error / wrong address\n");
    else if (ap_ctrl_before == 0x00000000u)
        printf("  <-- all-zero: plausible (ap_idle may not be set yet) but verify clock/reset if dispatch also fails\n");
    else
        printf("  <-- nonzero, non-all-F: IP is responding\n");

    uintptr_t desc_phys = FV_DDR_BASE + DESC_OFF;
    uintptr_t in_phys    = FV_DDR_BASE + IN_OFF;
    uintptr_t w_phys     = FV_DDR_BASE + W_OFF;
    uintptr_t b_phys     = FV_DDR_BASE + B_OFF;
    uintptr_t out_phys   = FV_DDR_BASE + OUT_OFF;
    uintptr_t out_written_phys = FV_DDR_BASE + OUT_WRITTEN_OFF;

    struct timespec tf0, tf1;
    clock_gettime(CLOCK_MONOTONIC, &tf0);
    mac_cache_flush(desc_phys, desc_size);
    mac_cache_flush(in_phys, in_size);
    mac_cache_flush(w_phys, w_size);
    mac_cache_flush(b_phys, b_size);
    clock_gettime(CLOCK_MONOTONIC, &tf1);
    double flush_ms = (tf1.tv_sec - tf0.tv_sec) * 1000.0 + (tf1.tv_nsec - tf0.tv_nsec) / 1e6;
    printf(">>> cache_flush(desc+in+w+b, total %zu bytes): %.3f ms\n",
           desc_size + in_size + w_size + b_size, flush_ms);

    /* ---- dispatch, by hand (not mac_run_layers) so we get the BOUNDED
     * wait for this first-ever call. ---- */
    volatile void *ctrl = NULL;
    {
        /* mac_driver_init() already mapped its own internal handle but
         * doesn't expose it -- re-open a R/W mapping here for the actual
         * register writes (small, harmless duplicate mapping). */
        int fd2 = open("/dev/mem", O_RDWR | O_SYNC);
        ctrl = mmap(NULL, MAC_ARRAY_MAP_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd2, MAC_ARRAY_CTRL_PHYS);
        if (ctrl == MAP_FAILED) { perror("mmap ctrl rw"); return 1; }
    }
    #define W32(off, val) (*(volatile uint32_t*)((char*)ctrl + (off)) = (uint32_t)(val))
    #define W64(lo, hi, addr) do { W32(lo, (uint32_t)(addr)); W32(hi, (uint32_t)((uint64_t)(addr) >> 32)); } while (0)

    W64(MAC_DESC_LO, MAC_DESC_HI, desc_phys);
    W32(MAC_N_LAYERS, 1);
    W64(MAC_IN_BASE_LO, MAC_IN_BASE_HI, in_phys);
    W64(MAC_W_BASE_LO, MAC_W_BASE_HI, w_phys);
    W64(MAC_B_BASE_LO, MAC_B_BASE_HI, b_phys);
    W64(MAC_OUT_BASE_LO, MAC_OUT_BASE_HI, out_phys);
    W64(MAC_OUT_WRITTEN_LO, MAC_OUT_WRITTEN_HI, out_written_phys);
    /* A3 MERGE round (2026-08-23, ZHR-92): in_base_wide -- PW_PATCH_HOIST
     * now unconditionally reads through this port (see mac_array.cpp), so
     * every PW dispatch needs it set, not just use_wide_path=1 layers.
     * Same physical region as in_base -- already flushed above via
     * mac_cache_flush(in_phys, in_size). */
    W64(MAC_IN_BASE_WIDE_LO, MAC_IN_BASE_WIDE_HI, in_phys);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    W32(MAC_AP_CTRL_OFFSET, MAC_AP_START);

    int timed_out = 0;
    long poll_count = 0;
    for (;;) {
        uint32_t v = *(volatile uint32_t*)((char*)ctrl + MAC_AP_CTRL_OFFSET);
        poll_count++;
        if (v & MAC_AP_DONE) break;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (elapsed_ms > 5000.0) { timed_out = 1; break; }
        usleep(1000);
    }
    printf(">>> poll_count = %ld (independent cross-check: poll_count * ~1.08ms measured usleep granularity = %.1f ms)\n",
           poll_count, poll_count * 1.0768);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    if (timed_out) {
        printf(">>> TIMEOUT after %.1f ms -- ap_done never set. DO NOT trust any output.\n", elapsed_ms);
        printf(">>> Check (cheapest first): bitstream actually loaded (re-check AP_CTRL read), "
               "PS7 FCLK0 enabled in this new block design, base address 0x%08lx still correct "
               "for THIS bitstream (re-check assign_bd_address / hwh if this bitstream differs "
               "from what was loaded).\n", (unsigned long)MAC_ARRAY_CTRL_PHYS);
        return 2;
    }
    printf(">>> ap_done set after %.2f ms (AP_START write -> ap_done observed, poll interval ~1ms)\n", elapsed_ms);

    struct timespec ti0, ti1;
    clock_gettime(CLOCK_MONOTONIC, &ti0);
    mac_cache_invalidate(out_phys, ref_size);
    mac_cache_invalidate(out_written_phys, sizeof(int32_t));
    clock_gettime(CLOCK_MONOTONIC, &ti1);
    double inval_ms = (ti1.tv_sec - ti0.tv_sec) * 1000.0 + (ti1.tv_nsec - ti0.tv_nsec) / 1e6;
    printf(">>> cache_invalidate(out+out_written, total %zu bytes): %.3f ms\n",
           ref_size + sizeof(int32_t), inval_ms);
    printf(">>> ns/byte over dispatch window (in+w+b+out=%zu bytes): %.1f ns/byte\n",
           in_size + w_size + b_size + ref_size,
           (elapsed_ms * 1e6) / (double)(in_size + w_size + b_size + ref_size));

    printf(">>> out_written[0] = %d\n", out_written_v[0]);
    if (out_written_v[0] == 0) {
        printf(">>> FAIL: out_written[0] is still 0 -- IP reported done but never performed the "
               "write-back (this is exactly defect-5's symptom class). Do not trust out.bin.\n");
        return 3;
    }

    /* "output actually changed from the poison pattern" check. */
    size_t unchanged = 0;
    for (size_t i = 0; i < ref_size; i++) if (out_v[i] == (uint8_t)0xA5) unchanged++;
    printf(">>> unchanged-from-poison bytes: %zu / %zu\n", unchanged, ref_size);

    /* byte-exact vs csim reference */
    uint8_t *ref = malloc(ref_size);
    snprintf(path, sizeof(path), "%s/ref_out.bin", dir);
    load_file(path, ref, ref_size);

    size_t mismatches = 0;
    int max_abs_diff = 0;
    for (size_t i = 0; i < ref_size; i++) {
        int diff = (int)(int8_t)out_v[i] - (int)(int8_t)ref[i];
        if (diff != 0) mismatches++;
        int ad = diff < 0 ? -diff : diff;
        if (ad > max_abs_diff) max_abs_diff = ad;
    }
    printf(">>> vs csim reference: mismatches = %zu / %zu, max_abs_diff = %d\n",
           mismatches, ref_size, max_abs_diff);
    printf(">>> %s\n", mismatches == 0 ? "PASS (byte-exact vs csim)" : "FAIL (numeric mismatch)");

    /* Always dump the real hardware output for offline distribution
     * analysis -- ZHR-92, 2026-08-22: "the fastest 5-minute check when
     * csim passes and hardware doesn't is where the mismatches cluster,
     * not guessing at mechanisms first." */
    {
        char out_path[600];
        snprintf(out_path, sizeof(out_path), "%s/actual_out.bin", dir);
        FILE *of = fopen(out_path, "wb");
        if (of) { fwrite(out_v, 1, ref_size, of); fclose(of); }
    }

    munmap((void*)ctrl, MAC_ARRAY_MAP_SIZE);
    munmap(dma_virt, FV_MAP_SIZE);
    close(fd_dma);
    mac_driver_exit();
    free(ref);

    return mismatches == 0 ? 0 : 4;
}
