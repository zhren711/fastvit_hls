/*================================================================
 * mac_array_driver.h -- A3 (ZHR-92, 2026-08-21): driver for the new
 * layer-controller + MAC array architecture (mac_array_top), replacing
 * the old op_code-dispatch fastvit_driver.h for board bring-up. Register
 * offsets are read directly from Vitis HLS's generated
 * fastvit_ip_v2/mac_array_poc_a3_axi/solution1/impl/misc/drivers/
 * mac_array_top_v1_0/src/xmac_array_top_hw.h -- not guessed.
 *
 * s_axi_control base address (0x40000000) confirmed from the real block
 * design's hw_handoff (mac_array_bd.hwh, C_S_AXI_CONTROL_BASEADDR) after
 * assign_bd_address -- same single-window convention the merged-IP
 * petalinux driver (fastvit_driver.h) already uses, just one IP instead
 * of one shared IP dispatching by op_code.
 *
 * Interface is now 6 pointers (desc/in_base/w_base/b_base/out_base/
 * out_written) + scalar n_layers, all over AXI-Lite -- desc[] itself is
 * an ARRAY OF LayerDescV2 living in DRAM, read by the IP's own m_axi
 * gmem_meta master. That is a new ARM->DRAM handoff point the old
 * architecture never had (old IP took scalars directly over AXI-Lite,
 * no descriptor table in DRAM) -- mac_run_layers() flushes it explicitly,
 * per the project's non-coherent-HP-port constraint (CLAUDE.md), designed
 * in from the first call, not bolted on after a symptom.
 *================================================================*/
#ifndef __MAC_ARRAY_DRIVER_H__
#define __MAC_ARRAY_DRIVER_H__

#include <stdint.h>
#include <stddef.h>

#define MAC_ARRAY_CTRL_PHYS   0x40000000UL
#define MAC_ARRAY_MAP_SIZE    0x10000UL

#define MAC_AP_CTRL_OFFSET        0x00
#define MAC_AP_START   (1u << 0)
#define MAC_AP_DONE    (1u << 1)
#define MAC_AP_IDLE    (1u << 2)

/* ZHR-92 (2026-08-30): gmem_meta ELIMINATED -- desc/out_written moved from
 * an m_axi-backed DRAM table + 64-bit base-address register to direct
 * s_axi_control registers (desc: 28 x 32-bit data registers, one per
 * MacLayerDesc field in declaration order; out_written: a single 32-bit
 * READ register + a vld control bit, no pointer at all). ALL of the
 * following offsets moved as a result -- re-derived from the freshly
 * generated fastvit_ip_v2/gmemmeta_elim1/s1/impl/ip/drivers/
 * mac_array_top_v1_0/src/xmac_array_top_hw.h, not computed by hand or
 * carried forward from the pre-elimination map (per this project's own
 * standing rule: every s_axilite port's address can move on ANY interface
 * change, always re-derive from the real generated header). MAC_N_LAYERS
 * is GONE -- the n_layers parameter no longer exists (every real dispatch
 * was already n_layers=1; the IP now processes exactly one descriptor per
 * ap_start, see mac_array.h's own header comment on mac_array_top). */
#define MAC_DESC_BASE         0x10   /* 28 x 4-byte registers, 0x10..0x7c,
                                       * one per MacLayerDesc/LayerDescV2
                                       * field in declaration order --
                                       * write field k to MAC_DESC_BASE+4*k. */
#define MAC_IN_BASE_LO        0x84
#define MAC_IN_BASE_HI        0x88
#define MAC_W_BASE_LO         0x90
#define MAC_W_BASE_HI         0x94
#define MAC_B_BASE_LO         0x9c
#define MAC_B_BASE_HI         0xa0
#define MAC_OUT_BASE_LO       0xa8
#define MAC_OUT_BASE_HI       0xac
/* out_written is no longer a DRAM pointer -- it's a plain scalar output
 * register the IP writes directly and the host reads after ap_done (same
 * "value stable at ap_done" contract mac_array_top's own `return` already
 * relied on). MAC_OUT_WRITTEN_CTRL's bit 0 is ap_vld (Read/COR) -- reading
 * MAC_OUT_WRITTEN_DATA after ap_done is sufficient in practice (ap_done
 * already implies the write happened), the vld bit is available but not
 * required by mac_run_layers below. */
#define MAC_OUT_WRITTEN_DATA  0xb4
#define MAC_OUT_WRITTEN_CTRL  0xb8
/* A3 MERGE round (2026-08-23, ZHR-92): in_base_wide, the 8th s_axi_control
 * pointer -- offset re-derived fresh for the gmem_meta-elimination build
 * (was 0x60/0x64 pre-elimination). Same physical DRAM region as in_base
 * (host always passes the identical pointer for both -- see
 * mac_array.cpp's own m_axi pragma comment); no separate cache-flush
 * range needed since in_flush_size already covers it. */
#define MAC_IN_BASE_WIDE_LO   0xc4
#define MAC_IN_BASE_WIDE_HI   0xc8
/* ZHR-92 angle-B (2026-08-24): out_burst, the 9th s_axi_control pointer --
 * offset re-derived fresh for the gmem_meta-elimination build (was
 * 0x6c/0x70 pre-elimination). Same physical DRAM region as out_base
 * (WRITEOUT's fast path and slow path both target the same output buffer,
 * just via two different views on the same bundle=gmem_act master) --
 * host must write the SAME address into both MAC_OUT_BASE_* and
 * MAC_OUT_BURST_*, or the fast path's writes go to whatever this register
 * defaults to. */
#define MAC_OUT_BURST_LO      0xd0
#define MAC_OUT_BURST_HI      0xd4

/* A3 row-hoist round (2026-08-25, ZHR-92): in_burst, ROW_READ's read-side
 * counterpart to out_burst -- offset re-derived fresh for the gmem_meta-
 * elimination build (was 0x78/0x7c pre-elimination). Same "shared bundle
 * != shared control register" trap as out_burst's own history. Same
 * physical DRAM region as in_base/in_base_wide (ROW_READ's burst path and
 * the slow in_base_wide reads share bundle=gmem_act) -- host must write
 * the SAME address into MAC_IN_BASE_WIDE_* and MAC_IN_BURST_*. */
#define MAC_IN_BURST_LO        0xdc
#define MAC_IN_BURST_HI        0xe0

/* Must stay byte-layout-identical to fastvit_ip_v2/mac_array.h's
 * LayerDescV2 (int fields, same order) -- this is now what gets written
 * field-by-field into the MAC_DESC_BASE register block (was DMA'd into
 * DRAM and read by the IP's gmem_meta master before the 2026-08-30
 * elimination), so the two structs are one contract, not independently
 * editable. */
typedef struct {
    int32_t op_type;
    int32_t cin, cout;
    int32_t h_in, w_in;
    int32_t k, stride, pad;
    int32_t fpg;
    int32_t out_shift;
    int32_t in_off, w_off, b_off, out_off;
    int32_t in2_off;
    int32_t h_out, w_out;
    int32_t n_row_tiles, n_col_tiles, n_ch_tiles;
    int32_t last_row_tile, last_col_tile, last_ch_tile;
    int32_t use_shift_table;
    int32_t shift_off;
    int32_t in_ch_stride, out_ch_stride;
    int32_t use_wide_path;  /* ZHR-92 (2026-08-29): was missing -- LayerDescV2's
                              * 28th field (appended 2026-08-23) was never mirrored
                              * here, despite this struct's own comment claiming
                              * byte-layout identity. Harmless until now only
                              * because use_wide_path is dead code in the current
                              * dispatch path; fixed as part of the desc-to-
                              * s_axilite conversion, which needs every field
                              * explicitly supplied. See CLAUDE.md's "a comment
                              * asserting an invariant..." entry. */
} MacLayerDesc;

/* 28 fields, all int32_t, no padding (verified: MacLayerDesc's own field
 * list above has exactly 28 int32_t members) -- MAC_DESC_NUM_FIELDS lets
 * the write loop and the _Static_assert below both reference one source
 * of truth instead of the literal 28 appearing twice and silently
 * drifting apart if a field is ever added again without updating both
 * (exactly the class of bug this constant exists to prevent -- see
 * CLAUDE.md's "a comment asserting an invariant..." entry, about this
 * exact struct). */
#define MAC_DESC_NUM_FIELDS  28
_Static_assert(sizeof(MacLayerDesc) == MAC_DESC_NUM_FIELDS * sizeof(int32_t),
               "MacLayerDesc field count/layout drifted from MAC_DESC_NUM_FIELDS "
               "-- update both together, and re-check against LayerDescV2.");

int  mac_driver_init(void);
void mac_driver_exit(void);

/* Writes desc's 28 fields directly to the MAC_DESC_BASE register block,
 * one 32-bit register per field, in MacLayerDesc's own declaration order
 * (field k -> MAC_DESC_BASE+4*k). Shared by mac_run_layers() and any test
 * harness that does its own inline per-entry dispatch instead of calling
 * mac_run_layers (mac_array_full_network_test.c, mac_array_single_op_
 * test.c) -- exported instead of duplicated, per this project's own
 * repeated "two places do the same thing, only one gets maintained" class
 * of bug (use_wide_path, mac_run_layers's unbounded wait, the full-
 * network-test's own duplicate poll loop -- see CLAUDE.md). Caller does
 * NOT need to mac_cache_flush desc -- it goes straight into
 * s_axi_control registers, never touches DRAM. */
void mac_write_desc(const MacLayerDesc *desc);

/* Cache sync (Zynq HP port non-coherent) -- same __builtin___clear_cache
 * technique as fastvit_driver.c's fv_cache_flush/invalidate (commit
 * 2cd8374, confirmed the real fix for the FinalDW stale-cache bug).
 * Reused verbatim, not reinvented. */
void mac_cache_flush(uintptr_t phys_addr, size_t size);      /* CPU -> FPGA */
void mac_cache_invalidate(uintptr_t phys_addr, size_t size); /* FPGA -> CPU */

void mac_wait_done(void);

/* Bounded poll for the FIRST-EVER hardware dispatch on this architecture
 * (ZHR-92, 2026-08-21) -- ZHR-10's precedent (an "HLS/Vivado all-green"
 * design that hung the real board) is exactly why an unbounded
 * mac_wait_done() must not be the first thing this new IP ever sees.
 * Returns 0 if ap_done set within timeout_ms, 1 on timeout (caller must
 * not trust any output in that case). Production dispatch (once this
 * architecture is trusted) should still use the unbounded mac_wait_done,
 * matching the existing fv_wait_done() convention. */
int mac_wait_done_timeout(int timeout_ms);

/* Dispatches ONE descriptor, given directly by the host (`desc`, already
 * populated by the caller -- ZHR-92 2026-08-30: no longer a DRAM pointer,
 * no longer plural, matching gmem_meta's elimination and the fact every
 * real dispatch was already n_layers=1). Writes `desc`'s 28 fields
 * directly to the MAC_DESC_BASE register block (mechanical, straight from
 * the host struct already in ARM memory -- no DRAM staging, no
 * mac_cache_flush needed for desc at all anymore, unlike the pre-
 * elimination version). Flushes the explicit [phys, phys+size) byte
 * ranges the caller names for in_base/w_base/b_base before ap_start
 * (every size is caller-supplied, not inferred -- an earlier draft of
 * this function reused out_check_size as a stand-in for the in_base
 * flush size and never flushed w_base/b_base at all, which is exactly the
 * class of in-range-looking, non-crashing cache bug CLAUDE.md's Zynq
 * HP-port note warns about; fixed before this was ever run, not after a
 * symptom). Invalidates the out_base region [out_check_off_phys,
 * +out_check_size) after ap_done. Returns the IP's own out_written
 * register value (1 iff the real output write happened, 0 = defect-5
 * symptom: IP reports ap_done but the write silently never happened) --
 * a direct register read now, not a DRAM array scan (mac_check_written,
 * which scanned out_written[0..n_layers), is retired along with n_layers
 * itself; had zero real callers even before this change). */
int mac_run_layers(
    const MacLayerDesc *desc,
    uintptr_t in_base_phys,  size_t in_flush_size,
    uintptr_t w_base_phys,   size_t w_flush_size,
    uintptr_t b_base_phys,   size_t b_flush_size,
    uintptr_t out_base_phys,
    uintptr_t out_check_off_phys, size_t out_check_size
);

#endif /* __MAC_ARRAY_DRIVER_H__ */
