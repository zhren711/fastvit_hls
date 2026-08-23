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

#define MAC_DESC_LO           0x10
#define MAC_DESC_HI           0x14
#define MAC_N_LAYERS          0x1c
#define MAC_IN_BASE_LO        0x24
#define MAC_IN_BASE_HI        0x28
#define MAC_W_BASE_LO         0x30
#define MAC_W_BASE_HI         0x34
#define MAC_B_BASE_LO         0x3c
#define MAC_B_BASE_HI         0x40
#define MAC_OUT_BASE_LO       0x48
#define MAC_OUT_BASE_HI       0x4c
#define MAC_OUT_WRITTEN_LO    0x54
#define MAC_OUT_WRITTEN_HI    0x58
/* A3 MERGE round (2026-08-23, ZHR-92): in_base_wide, the 8th s_axi_control
 * pointer, appended by HLS after out_written -- offset read directly from
 * solution19's generated xmac_array_top_hw.h (0x60/0x64), not guessed.
 * Same physical DRAM region as in_base (host always passes the identical
 * pointer for both -- see mac_array.cpp's own m_axi pragma comment); no
 * separate cache-flush range needed since in_flush_size already covers it. */
#define MAC_IN_BASE_WIDE_LO   0x60
#define MAC_IN_BASE_WIDE_HI   0x64

/* Must stay byte-layout-identical to fastvit_ip_v2/mac_array.h's
 * LayerDescV2 (int fields, same order) -- this is what actually gets DMA'd
 * into DRAM and read by the IP's gmem_meta master, so the two structs are
 * one contract, not independently editable. */
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
} MacLayerDesc;

int  mac_driver_init(void);
void mac_driver_exit(void);

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

/* Dispatches n_layers descriptors starting at desc_phys (already written
 * + to be flushed by this call). Flushes desc[] and the explicit
 * [phys, phys+size) byte ranges the caller names for in_base/w_base/
 * b_base before ap_start (every size is caller-supplied, not inferred --
 * an earlier draft of this function reused out_check_size as a stand-in
 * for the in_base flush size and never flushed w_base/b_base at all,
 * which is exactly the class of in-range-looking, non-crashing cache bug
 * CLAUDE.md's Zynq HP-port note warns about; fixed before this was ever
 * run, not after a symptom). Invalidates the out_base region
 * [out_check_off_phys, +out_check_size) and the out_written array after
 * ap_done. out_written_phys/n_layers let the caller self-check every
 * dispatched layer's write-back flag before trusting out_base, same
 * discipline mac_array_tb.cpp's csim testbench already applies -- this
 * is the on-board equivalent of that check, not a new invention. */
void mac_run_layers(
    uintptr_t desc_phys, int n_layers,
    uintptr_t in_base_phys,  size_t in_flush_size,
    uintptr_t w_base_phys,   size_t w_flush_size,
    uintptr_t b_base_phys,   size_t b_flush_size,
    uintptr_t out_base_phys,
    uintptr_t out_written_phys,
    uintptr_t out_check_off_phys, size_t out_check_size
);

/* Reads out_written[0..n_layers) after mac_run_layers() and returns 1 iff
 * every entry is nonzero, 0 otherwise (defect-5 detector: IP reports
 * ap_done but the real output write silently never happened). Caller must
 * have already invalidated out_written_phys (mac_run_layers does this). */
int mac_check_written(const int32_t *out_written_virt, int n_layers);

#endif /* __MAC_ARRAY_DRIVER_H__ */
