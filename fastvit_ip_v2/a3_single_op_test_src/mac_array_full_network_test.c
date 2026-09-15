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
/* ZHR-92 (2026-08-30): DESC_OFF/OUT_WRITTEN_OFF DRAM regions RETIRED --
 * gmem_meta elimination means desc is written straight into s_axi_control
 * registers (mac_write_desc(), no DRAM staging) and out_written is read
 * back from a single register (no DRAM array). desc_all.bin itself is
 * STILL needed (see below, host_desc[] is loaded from it) -- only the
 * DRAM COPY of it that used to exist at this offset is gone. */
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
    /* ZHR-92 (2026-08-24): stdout is fully-buffered (not line-buffered)
     * when it's a pipe rather than a tty -- exactly the case when this
     * runs over SSH -- so printf output can sit unflushed in libc's
     * internal buffer indefinitely. If the process hangs and gets killed,
     * everything since the last flush is lost, which is exactly what
     * happened investigating the original full-network hang (zero bytes
     * captured despite real dispatch activity). Force line-buffering so
     * every printf reaches the pipe immediately -- the scattered
     * fflush(stdout) calls below are now redundant but kept as explicit
     * documentation at the specific points that matter most. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s <bundle_dir containing desc_all.bin> [n_entries_limit]\n", argv[0]);
        return 1;
    }
    char path[600];
    const char *dir = argv[1];
    /* ZHR-92 (2026-08-24): bisection support -- run only entries 0..limit-1,
     * default N_HW_SEQ (all 82). Used to localize the full-network hang
     * (entries 0-15 pass in isolation individually, but no isolated
     * single-op test can expose a real-sequential-execution-only bug --
     * this bisects the REAL harness itself instead of building more
     * one-off bundles). */
    int n_limit = N_HW_SEQ;
    if (argc == 3) {
        n_limit = atoi(argv[2]);
        if (n_limit < 1 || n_limit > N_HW_SEQ) {
            fprintf(stderr, "n_entries_limit must be 1..%d, got %d\n", N_HW_SEQ, n_limit);
            return 1;
        }
    }
    printf(">>> running entries 0..%d (n_limit=%d)\n", n_limit - 1, n_limit);

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

    /* Poison the whole arena so "output changed" is a real check for
     * every entry, not a coincidence -- same discipline as the single-op
     * tests, just over the whole shared arena instead of one region. */
    memset(arena_v, 0xA5, 0x1D0000UL);
    load_file(stem_path, arena_v, stem_size);
    load_file(w_path, w_v, w_size);
    load_file(b_path, b_v, b_size);
    /* desc is dispatched straight from host_desc[] via mac_write_desc()
     * per-entry below -- no DRAM copy/flush needed anymore (gmem_meta is
     * gone). out_written is read back per-entry from a register, no DRAM
     * array to zero-init either. */

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

    /* Flushed ONCE, up front -- none of these are modified again during
     * the 82-entry loop (weights/bias/stem input are the network's fixed
     * initial state). desc no longer needs a flush at all (see above). */
    mac_cache_flush(w_phys, w_size);
    mac_cache_flush(b_phys, b_size);
    mac_cache_flush(arena_phys, 0x1D0000UL);

    int fd2 = open("/dev/mem", O_RDWR | O_SYNC);
    volatile void *ctrl = mmap(NULL, MAC_ARRAY_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, MAC_ARRAY_CTRL_PHYS);
    if (ctrl == MAP_FAILED) { perror("mmap ctrl rw"); return 1; }
    #define W32(off, val) (*(volatile uint32_t*)((char*)ctrl + (off)) = (uint32_t)(val))
    #define R32(off) (*(volatile uint32_t*)((char*)ctrl + (off)))
    #define W64(lo, hi, addr) do { W32(lo, (uint32_t)(addr)); W32(hi, (uint32_t)((uint64_t)(addr) >> 32)); } while (0)

    int written_ok[N_HW_SEQ];
    double entry_ms[N_HW_SEQ];
    /* ZHR-92 (2026-09-15): host-side gap BEFORE each entry (previous entry's
     * ap_done -> this entry's AP_START: descriptor/base-address register
     * writes, the two printf+fflush, and any checkpoint dump in between).
     * Answers "where does PL-total minus sum(entry_ms) go". */
    double gap_ms[N_HW_SEQ];
    struct timespec prev_e1;
    /* ZHR-92 (2026-09-15, host-gap round): everything that is NOT dispatch
     * is moved out of the timed window. The per-entry gap was measured at
     * 63us (48 register writes ~9us + two printf/fflush ~40us + reads) and
     * the six checkpoint dumps at 16.0ms (cache invalidate + fwrite of up to
     * 196KB to the board's filesystem) -- 20.8ms of the 287.5ms "PL total"
     * was harness file I/O, not inference. Now: the checkpoint is
     * invalidated and memcpy'd to RAM inside the loop (it must be captured
     * before a later entry overwrites the arena region) and written to disk
     * after t1; the per-entry "done" line is printed after t1 from the
     * arrays; only the one-line "dispatching" progress marker (needed to
     * name the entry if the board hangs) stays before AP_START. */
    uint8_t *ckpt_buf[6];
    uint32_t out_written_arr[N_HW_SEQ];
    int n_done = 0;
    for (int k = 0; k < N_CKPT; k++) {
        ckpt_buf[k] = (uint8_t*)malloc((size_t)g_ckpts[k].size);
        if (!ckpt_buf[k]) { fprintf(stderr, "malloc ckpt_buf[%d] failed\n", k); return 1; }
    }
    int ckpt_cursor = 0;
    int any_fail = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    prev_e1 = t0;

    for (int i = 0; i < n_limit; i++) {
        /* ZHR-92 (2026-08-30): desc dispatched straight from host_desc[i]
         * via mac_write_desc() -- no DRAM pointer, no n_layers (gmem_meta
         * is gone). mac_driver_init() above already mmap'd mac_ctrl;
         * mac_write_desc() writes through that mapping, this loop's own
         * W32/W64 macros write through a separate mapping to the SAME
         * physical control region (harmless -- MMIO, not cached RAM). */
        mac_write_desc(&host_desc[i]);
        W64(MAC_IN_BASE_LO, MAC_IN_BASE_HI, arena_phys);
        W64(MAC_W_BASE_LO, MAC_W_BASE_HI, w_phys);
        W64(MAC_B_BASE_LO, MAC_B_BASE_HI, b_phys);
        W64(MAC_OUT_BASE_LO, MAC_OUT_BASE_HI, arena_phys);
        W64(MAC_IN_BASE_WIDE_LO, MAC_IN_BASE_WIDE_HI, arena_phys);
        /* A3 row-hoist round (2026-08-25, ZHR-92): in_burst -- ROW_READ's
         * read-side counterpart to out_burst, same physical region as
         * in_base/in_base_wide, same "shared bundle != shared control
         * register" trap. Flagged and fixed before this round's board
         * test, not after. */
        W64(MAC_IN_BURST_LO, MAC_IN_BURST_HI, arena_phys);
        /* ZHR-92 angle-B (2026-08-24): out_burst -- WRITEOUT's fast path
         * writes through this port, same physical region as out_base
         * despite sharing bundle=gmem_act (shared bundle != shared
         * control register -- see CLAUDE.md). Every dispatch needs it
         * set; csim cannot catch a missing write here at all. */
        W64(MAC_OUT_BURST_LO, MAC_OUT_BURST_HI, arena_phys);
        /* ZHR-92 round (2026-09-07): elemwise_in_burst/elemwise_out_burst --
         * run_gelu/run_add's own new burst ports (ELEMWISE_BURST), same
         * shared-bundle/separate-register trap as in_burst/out_burst above.
         * Found the hard way: a real board hang (elemwise_in_burst reading
         * an unprogrammed address, on entry[0]'s own GELU dispatch) before
         * this write existed. */
        W64(MAC_ELEMWISE_IN_BURST_LO, MAC_ELEMWISE_IN_BURST_HI, arena_phys);
        W64(MAC_ELEMWISE_OUT_BURST_LO, MAC_ELEMWISE_OUT_BURST_HI, arena_phys);
        /* ZHR-92 round (2026-09-07): dw_in_burst -- dwr_prefetch_channel's
         * new burst port (DWR_INPUT_BURST), same shared-bundle/separate-
         * register trap as every burst port above. Wired up before this
         * round's first board attempt this time, not after a hang. */
        W64(MAC_DW_IN_BURST_LO, MAC_DW_IN_BURST_HI, arena_phys);
        W64(MAC_W_BURST_LO, MAC_W_BURST_HI, w_phys);   /* PW_WHOIST_WIDE port, own register */

        /* ZHR-92 (2026-08-24): print+flush BEFORE dispatch too -- if this
         * entry is the one that hangs, the process gets killed and any
         * buffered-but-unflushed output is lost. Printing the "about to
         * dispatch" line first, flushed immediately, guarantees we know
         * which entry was in flight even if nothing after this line ever
         * prints. */
        printf(">>> [%2d] op_type=%d cin=%d cout=%d h_in=%d w_in=%d -- dispatching...\n",
               i, host_desc[i].op_type, host_desc[i].cin, host_desc[i].cout,
               host_desc[i].h_in, host_desc[i].w_in);
        fflush(stdout);

        struct timespec e0, e1;
        clock_gettime(CLOCK_MONOTONIC, &e0);
        gap_ms[i] = (e0.tv_sec - prev_e1.tv_sec) * 1000.0 + (e0.tv_nsec - prev_e1.tv_nsec) / 1e6;
        W32(MAC_AP_CTRL_OFFSET, MAC_AP_START);

        /* ZHR-92 (2026-08-28): this used to be its own inline poll loop
         * (usleep(500), duplicating mac_wait_done_timeout()'s usleep(1000)
         * in mac_array_driver.c) -- a third instance of the "two paths do
         * the same thing, only one is real" class this project has hit
         * before (use_wide_path, mac_run_layers's unbounded wait; see
         * CLAUDE.md). mac_driver_init() already ran above, so mac_ctrl is
         * valid; call the shared function instead of maintaining a second
         * implementation. Timeout unchanged at 30000ms (ZHR-92 2026-08-24
         * precedent, ~100x the slowest real entry measured so far). Note
         * for future measurement rounds: this changes the per-entry poll
         * granularity from 0.5ms to 1ms -- the ~65ms/1.79% fixed-overhead
         * floor measured 2026-08-28 was on the OLD 0.5ms loop and will
         * shift slightly (not re-measured after this change).
         * ZHR-92 (2026-09-15): mac_wait_done_timeout() is now a BUSY-POLL
         * (~1.3us resolution); until then its usleep(1000) made every
         * per-entry time below a multiple of ~1.08ms. */
        int timed_out = mac_wait_done_timeout(30000);
        clock_gettime(CLOCK_MONOTONIC, &e1);
        entry_ms[i] = (e1.tv_sec - e0.tv_sec) * 1000.0 + (e1.tv_nsec - e0.tv_nsec) / 1e6;
        prev_e1 = e1;

        if (timed_out) {
            printf(">>> [%2d] TIMEOUT after 30000ms -- ABORTING, do not trust anything past this point\n", i);
            fflush(stdout);
            fprintf(stderr, ">>> TIMEOUT at entry %d (op_type=%d) after 30000ms -- ABORTING, do not trust anything past this point\n",
                    i, host_desc[i].op_type);
            written_ok[i] = 0;
            any_fail = 1;
            break;
        }

        /* out_written is a plain register now -- read directly, no DRAM
         * invalidate needed (MMIO device space is never CPU-cached the
         * way DRAM is). */
        uint32_t out_written_val = R32(MAC_OUT_WRITTEN_DATA);
        written_ok[i] = (out_written_val != 0);
        /* (2026-09-15) the "done" line is now printed after t1; the NEXT
         * entry's "dispatching" marker still names a hung entry, and
         * reaching it implies this one completed. */
        out_written_arr[i] = out_written_val;
        n_done = i + 1;
        if (!written_ok[i]) {
            fprintf(stderr, ">>> entry %d: out_written[%d]=0 -- defect-5 symptom (ap_done set, write never happened)\n", i, i);
            any_fail = 1;
        }

        while (ckpt_cursor < N_CKPT && g_ckpts[ckpt_cursor].seq_index == i) {
            struct CkptEntry *ck = &g_ckpts[ckpt_cursor];
            uintptr_t ck_phys = arena_phys + (uintptr_t)ck->out_off;
            mac_cache_invalidate(ck_phys, (size_t)ck->size);
            memcpy(ckpt_buf[ckpt_cursor], arena_v + ck->out_off, (size_t)ck->size);
            ckpt_cursor++;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double total_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    /* Deferred per-entry report (same line format as before -- the
     * decomposition tool parses "done: X.XXXms"). */
    for (int i = 0; i < n_done; i++) {
        printf(">>> [%2d] done: %.3fms, gap_before=%.3fms, out_written=%u%s\n",
               i, entry_ms[i], gap_ms[i], out_written_arr[i], written_ok[i] ? "" : "  <-- FAIL (defect-5 symptom)");
    }
    /* Deferred checkpoint files. */
    for (int k = 0; k < ckpt_cursor; k++) {
        struct CkptEntry *ck = &g_ckpts[k];
        char out_path[700];
        snprintf(out_path, sizeof(out_path), "%s/ckpt_board_%s.bin", dir, ck->tag);
        FILE *of = fopen(out_path, "wb");
        if (of) { fwrite(ckpt_buf[k], 1, (size_t)ck->size, of); fclose(of); }
        printf("  [%2d] dumped checkpoint '%s' -> %s (%d bytes) [captured in-loop, written after t1]\n",
               ck->seq_index, ck->tag, out_path, ck->size);
    }
    fflush(stdout);

    /* Dump entry 81's raw output too (the trailing GELU past the
     * established 7-checkpoint 'se' endpoint) -- no accuracy reference
     * wired up for it yet, kept for future use, not part of this round's
     * pass/fail judgment (see ZHR-92 for why 'se' is this round's
     * end-to-end comparison point). */
    /* ZHR-92 (2026-08-24): only meaningful when the run actually reached
     * entry 81 (n_limit==N_HW_SEQ) -- when bisecting with a smaller
     * n_limit, host_desc[81] is valid (desc_all.bin is always loaded in
     * full) but its ARENA output was never written by the IP, so dumping
     * it would just be stale/poison data, not a real result. */
    if (n_limit == N_HW_SEQ && (!any_fail || ckpt_cursor >= N_CKPT)) {
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
    for (int i = 0; i < n_limit; i++) if (written_ok[i]) n_ok++;

    /* top-10 slowest entries, by simple selection */
    printf("\n>>> top 10 most expensive entries (real, this run):\n");
    int used[N_HW_SEQ]; memset(used, 0, sizeof(used));
    for (int rank = 0; rank < 10 && rank < n_limit; rank++) {
        int best = -1;
        for (int i = 0; i < n_limit; i++) {
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

    printf("\n>>> out_written check: %d/%d entries confirmed written (of %d dispatched, n_limit=%d)\n",
           n_ok, n_limit, n_limit, n_limit);
    printf(">>> checkpoints dumped: %d/%d\n", ckpt_cursor, N_CKPT);
    printf(">>> PL-side total (AP_START entry[0] -> ap_done entry[%d] or abort): %.2f ms\n", n_limit - 1, total_ms);
    printf(">>> %s\n", (n_ok == n_limit && !any_fail) ? "PASS -- all dispatched entries written" : "INCOMPLETE -- see failures above");

    munmap((void*)ctrl, MAC_ARRAY_MAP_SIZE);
    munmap(dma_virt, MAP_SIZE);
    close(fd_dma);
    close(fd2);
    mac_driver_exit();

    return (n_ok == n_limit && !any_fail) ? 0 : 2;
}
