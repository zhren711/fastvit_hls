/*================================================================
 * mac_array_single_op_test_add.c -- A3 end-to-end step 1 (ZHR-92,
 * 2026-08-22): first-ever ADD dispatch on real hardware. Priority op this
 * round -- defect-5 (Add write-back, root cause unknown since the OLD
 * architecture) can only be exposed by actually running Add on real
 * hardware, which this project has never done on the NEW architecture
 * until now.
 *
 * Same discipline as the PW round: byte-exact vs a real csim reference
 * (not synthetic), explicit out_written self-check, poison pattern on
 * the output buffer before dispatch so "did it actually write" is a real
 * test, not a coincidence of a zeroed buffer.
 *
 * 用法: mac_array_single_op_test_add <bundle_dir containing desc.bin,
 *       in.bin, ref_out.bin>
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
#define FV_MAP_SIZE   0x100000UL

#define DESC_OFF          0x00000UL
#define IN_OFF            0x10000UL   /* holds op0 then op1, concatenated */
#define OUT_OFF           0x70000UL
#define OUT_WRITTEN_OFF   0xB0000UL

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
    snprintf(path, sizeof(path), "%s/ref_out.bin", dir);
    size_t ref_size = file_size(path);

    printf(">>> bundle sizes: desc=%zu in=%zu ref_out=%zu\n", desc_size, in_size, ref_size);

    int fd_dma = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_dma < 0) { perror("open /dev/mem (dma)"); return 1; }
    void *dma_virt = mmap(NULL, FV_MAP_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd_dma, FV_DDR_BASE);
    if (dma_virt == MAP_FAILED) { perror("mmap dma region"); return 1; }

    uint8_t *desc_v = (uint8_t*)dma_virt + DESC_OFF;
    uint8_t *in_v   = (uint8_t*)dma_virt + IN_OFF;
    uint8_t *out_v  = (uint8_t*)dma_virt + OUT_OFF;
    int32_t *out_written_v = (int32_t*)((uint8_t*)dma_virt + OUT_WRITTEN_OFF);

    snprintf(path, sizeof(path), "%s/desc.bin", dir);
    load_file(path, desc_v, desc_size);
    snprintf(path, sizeof(path), "%s/in.bin", dir);
    load_file(path, in_v, in_size);

    memset(out_v, 0xA5, ref_size);
    out_written_v[0] = 0;

    if (mac_driver_init() != 0) {
        fprintf(stderr, "mac_driver_init failed\n");
        return 1;
    }

    uintptr_t desc_phys = FV_DDR_BASE + DESC_OFF;
    uintptr_t in_phys    = FV_DDR_BASE + IN_OFF;
    uintptr_t out_phys   = FV_DDR_BASE + OUT_OFF;
    uintptr_t out_written_phys = FV_DDR_BASE + OUT_WRITTEN_OFF;

    mac_cache_flush(desc_phys, desc_size);
    mac_cache_flush(in_phys, in_size);
    mac_cache_flush(out_phys, ref_size);           /* the poison pattern */
    mac_cache_flush(out_written_phys, sizeof(int32_t));

    int fd2 = open("/dev/mem", O_RDWR | O_SYNC);
    volatile void *ctrl = mmap(NULL, MAC_ARRAY_MAP_SIZE, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd2, MAC_ARRAY_CTRL_PHYS);
    if (ctrl == MAP_FAILED) { perror("mmap ctrl rw"); return 1; }
    #define W32(off, val) (*(volatile uint32_t*)((char*)ctrl + (off)) = (uint32_t)(val))
    #define W64(lo, hi, addr) do { W32(lo, (uint32_t)(addr)); W32(hi, (uint32_t)((uint64_t)(addr) >> 32)); } while (0)

    /* ADD never touches w_base/b_base -- point them at the same region as
     * in_base (harmless, never dereferenced by run_add). */
    W64(MAC_DESC_LO, MAC_DESC_HI, desc_phys);
    W32(MAC_N_LAYERS, 1);
    W64(MAC_IN_BASE_LO, MAC_IN_BASE_HI, in_phys);
    W64(MAC_W_BASE_LO, MAC_W_BASE_HI, in_phys);
    W64(MAC_B_BASE_LO, MAC_B_BASE_HI, in_phys);
    W64(MAC_OUT_BASE_LO, MAC_OUT_BASE_HI, out_phys);
    W64(MAC_OUT_WRITTEN_LO, MAC_OUT_WRITTEN_HI, out_written_phys);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    W32(MAC_AP_CTRL_OFFSET, MAC_AP_START);

    int timed_out = 0;
    for (;;) {
        uint32_t v = *(volatile uint32_t*)((char*)ctrl + MAC_AP_CTRL_OFFSET);
        if (v & MAC_AP_DONE) break;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (elapsed_ms > 5000.0) { timed_out = 1; break; }
        usleep(1000);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    if (timed_out) {
        printf(">>> TIMEOUT after %.1f ms -- ap_done never set. DO NOT trust any output.\n", elapsed_ms);
        return 2;
    }
    printf(">>> ap_done set after %.2f ms\n", elapsed_ms);

    mac_cache_invalidate(out_phys, ref_size);
    mac_cache_invalidate(out_written_phys, sizeof(int32_t));

    printf(">>> out_written[0] = %d\n", out_written_v[0]);
    if (out_written_v[0] == 0) {
        printf(">>> FAIL: out_written[0] is still 0 -- IP reported done but never performed the "
               "write-back. THIS IS DEFECT-5'S EXACT SYMPTOM. Do not trust out.bin.\n");
        return 3;
    }

    size_t unchanged = 0;
    for (size_t i = 0; i < ref_size; i++) if (out_v[i] == (uint8_t)0xA5) unchanged++;
    printf(">>> unchanged-from-poison bytes: %zu / %zu\n", unchanged, ref_size);

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

    munmap((void*)ctrl, MAC_ARRAY_MAP_SIZE);
    munmap(dma_virt, FV_MAP_SIZE);
    close(fd_dma);
    mac_driver_exit();
    free(ref);

    return mismatches == 0 ? 0 : 4;
}
