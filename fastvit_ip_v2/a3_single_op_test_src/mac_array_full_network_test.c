/*================================================================
 * mac_array_full_network_test.c -- A3 first full 82-entry end-to-end
 * board run (ZHR-92, 2026-08-23). Dispatches the REAL hardware sequence
 * ONE ENTRY AT A TIME (matching mac_array_ckpt_dump.cpp's own csim
 * approach and rationale -- behaviorally identical to a single 82-entry
 * n_layers call, just with observation points a combined dispatch
 * couldn't give), sharing ONE flat DRAM arena for in_base/out_base
 * across all 82 entries (Route C -- the real chain's own convention,
 * confirmed from mac_array_ckpt_desc.h's real in_off/out_off values,
 * which reuse the SAME small set of region offsets as later entries
 * overwrite earlier ones' scratch space).
 *
 * Four requirements from the pre-registration (ZHR-92):
 *   1. Dual accuracy reference -- this program only DUMPS checkpoints
 *      (stage1-4, finaldw, se) to disk; a separate Python script computes
 *      cosine similarity against BOTH ckpt_hw_*.bin (csim fixed-point,
 *      expect ~1.0) and ckpt_ref_*.npy (ONNX float32, expect ~0.6325).
 *   2. Segmented checkpoints -- reuses g_ckpts[] from mac_array_ckpt_desc.h
 *      verbatim (the SAME 6 tag/seq_index/out_off/size entries csim
 *      already uses), dumped INTERLEAVED with dispatch (not after the
 *      whole run) because later entries reuse (overwrite) earlier
 *      checkpoints' own memory (ping-pong MAIN0/MAIN1 reuse).
 *   3. Defect-5 style self-verification -- out_written[i] invalidated
 *      and checked EXPLICITLY for every i in 0..81, not assumed from
 *      ap_done alone; any zero is reported by index, run continues
 *      (not aborted) so a single bad entry doesn't hide information
 *      about the rest.
 *   4. Real per-entry cache flush before dispatch, invalidate after --
 *      only what's ACTUALLY new since the last flush (desc[i] itself
 *      needs no flush -- desc_all is flushed once, up front, since it's
 *      never modified after that).
 *
 * 用法: mac_array_full_network_test <bundle_dir containing desc_all.bin>
 *================================================================*/
#include "mac_array_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#define FV_DDR_BASE     0x10000000UL
#define ARENA_OFF       0x000000UL   /* activation arena, TOTAL_BYTES=1,819,392 real need */
#define W_OFF           0x200000UL   /* weights, ckpt_weights_flat.bin = 3,028,176 bytes */
#define B_OFF           0x500000UL   /* bias, ckpt_bias_flat.bin = 54,528 bytes */
#define DESC_OFF        0x510000UL   /* 82 x MacLayerDesc = 8,856 bytes */
#define OUT_WRITTEN_OFF 0x520000UL   /* 82 x int32 = 328 bytes */
#define MAP_SIZE        0x600000UL   /* 6MB window, generous margin over all regions */

#define N_HW_SEQ 82

struct CkptEntry { const char *tag; int seq_index; int out_off; int size; };
static const int N_CKPT = 6;
static struct CkptEntry g_ckpts[6] = {
    { "stage1",  16, 786432, 196608 },
    { "stage2",  31, 0,      98304  },
    { "stage3",  58, 786432, 49152  },
    { "stage4",  73, 0,      24576  },
    { "finaldw", 74, 1769472,49152  },
    { "se",      80, 786432, 49152  },
};

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
        fprintf(stderr, "usage: %s <bundle_dir containing desc_all.bin>\n", argv[0]);
        return 1;
    }
    char path[600];
    const char *dir = argv[1];

    snprintf(path, sizeof(path), "%s/desc_all.bin", dir);
    size_t desc_size = file_size(path);
    if (desc_size != (size_t)N_HW_SEQ * sizeof(MacLayerDesc)) {
        fprintf(stderr, "desc_all.bin size %zu != expected %zu\n", desc_size, (size_t)N_HW_SEQ * sizeof(MacLayerDesc));
        return 1;
    }
    MacLayerDesc host_desc[N_HW_SEQ];
    load_file(path, host_desc, desc_size);

    char w_path[600], b_path[600], stem_path[600];
    snprintf(w_path, sizeof(w_path), "%s/ckpt_weights_flat.bin", dir);
    snprintf(b_path, sizeof(b_path), "%s/ckpt_bias_flat.bin", dir);
    snprintf(stem_path, sizeof(stem_path), "%s/stem_output_0000.bin", dir);
    size_t w_size = file_size(w_path);
    size_t b_size = file_size(b_path);
    size_t stem_size = file_size(stem_path);
    printf(">>> desc_all=%zu bytes (%d entries), weights=%zu bytes, bias=%zu bytes, stem=%zu bytes\n",
           desc_size, N_HW_SEQ, w_size, b_size, stem_size);

    int fd_dma = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_dma < 0) { perror("open /dev/mem (dma)"); return 1; }
    void *dma_virt = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dma, FV_DDR_BASE);
    if (dma_virt == MAP_FAILED) { perror("mmap dma region"); return 1; }

    uint8_t *arena_v      = (uint8_t*)dma_virt + ARENA_OFF;
    uint8_t *w_v          = (uint8_t*)dma_virt + W_OFF;
    uint8_t *b_v          = (uint8_t*)dma_virt + B_OFF;
    uint8_t *desc_v       = (uint8_t*)dma_virt + DESC_OFF;
    int32_t *out_written_v= (int32_t*)((uint8_t*)dma_virt + OUT_WRITTEN_OFF);

    /* Poison the whole arena so "output changed" is a real check for
     * every entry, not a coincidence -- same discipline as the single-op
     * tests, just over the whole shared arena instead of one region. */
    memset(arena_v, 0xA5, 0x1D0000UL);
    load_file(stem_path, arena_v, stem_size);
    load_file(w_path, w_v, w_size);
    load_file(b_path, b_v, b_size);
    memcpy(desc_v, host_desc, desc_size);
    memset(out_written_v, 0, (size_t)N_HW_SEQ * sizeof(int32_t));

    if (mac_driver_init() != 0) { fprintf(stderr, "mac_driver_init failed\n"); return 1; }
    int fd_ctrl = open("/dev/mem", O_RDONLY | O_SYNC);
    void *ctrl_probe = mmap(NULL, MAC_ARRAY_MAP_SIZE, PROT_READ, MAP_SHARED, fd_ctrl, MAC_ARRAY_CTRL_PHYS);
    uint32_t ap_ctrl_before = *(volatile uint32_t*)((char*)ctrl_probe + MAC_AP_CTRL_OFFSET);
    munmap(ctrl_probe, MAC_ARRAY_MAP_SIZE);
    close(fd_ctrl);
    printf(">>> PRE-FLIGHT: AP_CTRL = 0x%08x %s\n", ap_ctrl_before,
           ap_ctrl_before == 0xFFFFFFFFu ? "<-- ALL-F, bitstream not loaded?" : "<-- responding");

    uintptr_t arena_phys       = FV_DDR_BASE + ARENA_OFF;
    uintptr_t w_phys           = FV_DDR_BASE + W_OFF;
    uintptr_t b_phys           = FV_DDR_BASE + B_OFF;
    uintptr_t desc_phys        = FV_DDR_BASE + DESC_OFF;
    uintptr_t out_written_phys = FV_DDR_BASE + OUT_WRITTEN_OFF;

    /* Flushed ONCE, up front -- none of these are modified again during
     * the 82-entry loop (desc_all is read-only once written; weights/bias/
     * stem input are the network's fixed initial state). */
    mac_cache_flush(desc_phys, desc_size);
    mac_cache_flush(w_phys, w_size);
    mac_cache_flush(b_phys, b_size);
    mac_cache_flush(arena_phys, 0x1D0000UL);

    int fd2 = open("/dev/mem", O_RDWR | O_SYNC);
    volatile void *ctrl = mmap(NULL, MAC_ARRAY_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, MAC_ARRAY_CTRL_PHYS);
    if (ctrl == MAP_FAILED) { perror("mmap ctrl rw"); return 1; }
    #define W32(off, val) (*(volatile uint32_t*)((char*)ctrl + (off)) = (uint32_t)(val))
    #define W64(lo, hi, addr) do { W32(lo, (uint32_t)(addr)); W32(hi, (uint32_t)((uint64_t)(addr) >> 32)); } while (0)

    int written_ok[N_HW_SEQ];
    double entry_ms[N_HW_SEQ];
    int ckpt_cursor = 0;
    int any_fail = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < N_HW_SEQ; i++) {
        uintptr_t this_desc_phys = desc_phys + (uintptr_t)i * sizeof(MacLayerDesc);
        uintptr_t this_written_phys = out_written_phys + (uintptr_t)i * sizeof(int32_t);

        W64(MAC_DESC_LO, MAC_DESC_HI, this_desc_phys);
        W32(MAC_N_LAYERS, 1);
        W64(MAC_IN_BASE_LO, MAC_IN_BASE_HI, arena_phys);
        W64(MAC_W_BASE_LO, MAC_W_BASE_HI, w_phys);
        W64(MAC_B_BASE_LO, MAC_B_BASE_HI, b_phys);
        W64(MAC_OUT_BASE_LO, MAC_OUT_BASE_HI, arena_phys);
        W64(MAC_OUT_WRITTEN_LO, MAC_OUT_WRITTEN_HI, this_written_phys);
        W64(MAC_IN_BASE_WIDE_LO, MAC_IN_BASE_WIDE_HI, arena_phys);

        struct timespec e0, e1;
        clock_gettime(CLOCK_MONOTONIC, &e0);
        W32(MAC_AP_CTRL_OFFSET, MAC_AP_START);

        int timed_out = 0;
        for (;;) {
            uint32_t v = *(volatile uint32_t*)((char*)ctrl + MAC_AP_CTRL_OFFSET);
            if (v & MAC_AP_DONE) break;
            clock_gettime(CLOCK_MONOTONIC, &e1);
            double elapsed_ms = (e1.tv_sec - e0.tv_sec) * 1000.0 + (e1.tv_nsec - e0.tv_nsec) / 1e6;
            if (elapsed_ms > 8000.0) { timed_out = 1; break; }
            usleep(500);
        }
        clock_gettime(CLOCK_MONOTONIC, &e1);
        entry_ms[i] = (e1.tv_sec - e0.tv_sec) * 1000.0 + (e1.tv_nsec - e0.tv_nsec) / 1e6;

        if (timed_out) {
            fprintf(stderr, ">>> TIMEOUT at entry %d (op_type=%d) after 8000ms -- ABORTING, do not trust anything past this point\n",
                    i, host_desc[i].op_type);
            written_ok[i] = 0;
            any_fail = 1;
            break;
        }

        mac_cache_invalidate(this_written_phys, sizeof(int32_t));
        written_ok[i] = (out_written_v[i] != 0);
        if (!written_ok[i]) {
            fprintf(stderr, ">>> entry %d: out_written[%d]=0 -- defect-5 symptom (ap_done set, write never happened)\n", i, i);
            any_fail = 1;
        }

        while (ckpt_cursor < N_CKPT && g_ckpts[ckpt_cursor].seq_index == i) {
            struct CkptEntry *ck = &g_ckpts[ckpt_cursor];
            uintptr_t ck_phys = arena_phys + (uintptr_t)ck->out_off;
            mac_cache_invalidate(ck_phys, (size_t)ck->size);
            char out_path[700];
            snprintf(out_path, sizeof(out_path), "%s/ckpt_board_%s.bin", dir, ck->tag);
            FILE *of = fopen(out_path, "wb");
            if (of) { fwrite(arena_v + ck->out_off, 1, (size_t)ck->size, of); fclose(of); }
            printf("  [%2d] dumped checkpoint '%s' -> %s (%d bytes)\n", i, ck->tag, out_path, ck->size);
            ckpt_cursor++;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double total_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    /* Dump entry 81's raw output too (the trailing GELU past the
     * established 7-checkpoint 'se' endpoint) -- no accuracy reference
     * wired up for it yet, kept for future use, not part of this round's
     * pass/fail judgment (see ZHR-92 for why 'se' is this round's
     * end-to-end comparison point). */
    if (!any_fail || ckpt_cursor >= N_CKPT) {
        uintptr_t final_phys = arena_phys + (uintptr_t)host_desc[N_HW_SEQ - 1].out_off;
        int final_size = host_desc[N_HW_SEQ - 1].cin * host_desc[N_HW_SEQ - 1].h_in * host_desc[N_HW_SEQ - 1].w_in;
        mac_cache_invalidate(final_phys, (size_t)final_size);
        char out_path[700];
        snprintf(out_path, sizeof(out_path), "%s/entry81_final.bin", dir);
        FILE *of = fopen(out_path, "wb");
        if (of) { fwrite(arena_v + host_desc[N_HW_SEQ - 1].out_off, 1, (size_t)final_size, of); fclose(of); }
        printf(">>> dumped entry[81] (final GELU) raw output -> %s (%d bytes)\n", out_path, final_size);
    }

    int n_ok = 0;
    for (int i = 0; i < N_HW_SEQ; i++) if (written_ok[i]) n_ok++;

    /* top-10 slowest entries, by simple selection (N=82, no need for
     * anything fancier) */
    printf("\n>>> top 10 most expensive entries (real, this run):\n");
    int used[N_HW_SEQ]; memset(used, 0, sizeof(used));
    for (int rank = 0; rank < 10 && rank < N_HW_SEQ; rank++) {
        int best = -1;
        for (int i = 0; i < N_HW_SEQ; i++) {
            if (!written_ok[i] && !any_fail) continue;  /* skip past-abort entries */
            if (used[i]) continue;
            if (best == -1 || entry_ms[i] > entry_ms[best]) best = i;
        }
        if (best == -1) break;
        used[best] = 1;
        printf("    [%2d] op_type=%d cin=%d cout=%d h=%d w=%d  %8.2f ms\n",
               best, host_desc[best].op_type, host_desc[best].cin, host_desc[best].cout,
               host_desc[best].h_in, host_desc[best].w_in, entry_ms[best]);
    }

    printf("\n>>> out_written check: %d/%d entries confirmed written\n", n_ok, N_HW_SEQ);
    printf(">>> checkpoints dumped: %d/%d\n", ckpt_cursor, N_CKPT);
    printf(">>> PL-side total (AP_START entry[0] -> ap_done entry[81] or abort): %.2f ms\n", total_ms);
    printf(">>> %s\n", (n_ok == N_HW_SEQ && ckpt_cursor == N_CKPT) ? "PASS -- all entries written, all checkpoints captured" : "INCOMPLETE -- see failures above");

    munmap((void*)ctrl, MAC_ARRAY_MAP_SIZE);
    munmap(dma_virt, MAP_SIZE);
    close(fd_dma);
    close(fd2);
    mac_driver_exit();

    return (n_ok == N_HW_SEQ && ckpt_cursor == N_CKPT) ? 0 : 2;
}
