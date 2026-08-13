// ==============================================================
// fastvit_top_tierb.v -- Tier B hand-written top level (FIRST VERTICAL
// SLICE: op_code=OP_ADD only; conv/dwconv/pwconv/gelu paths not yet
// wired up -- see C:\Users\zhren\.claude\plans\typed-knitting-nygaard.md
// Phase B2 "build order" note).
//
// Replaces the HLS-generated fastvit_ip.v's op_code-dispatch/AXI-mux
// glue (the proven 200MHz bottleneck, see project memory) while
// reusing, UNMODIFIED, every HLS-generated black box that was NOT the
// bottleneck:
//   - fastvit_ip_control_s_axi / fastvit_ip_ctrl_s_axi (AXI4-Lite
//     register files, exact same register map as xfastvit_ip_hw.h)
//   - fastvit_ip_add_worker (+ its Pipeline_ADD_LOOP sub-module)
//   - fastvit_ip_mul_32s_32s_32_5_1 (add_worker's internal shared
//     multiplier stub, private copies here instead of top-shared)
//   - fastvit_ip_gmem0_m_axi / gmem1_m_axi / gmem3_m_axi (per-bundle
//     AXI4 burst/width adapters) -- one INSTANCE PER WORKER instead of
//     one shared instance per bundle, so add_worker gets its own
//     private, unshared physical AXI4 master ports (no mux needed on
//     the data path at all; arbitration moves to Vivado's SmartConnect
//     at BD integration time, Phase B4).
//
// op_code dispatch: each op gets its own independently-registered
// enable (en_conv/en_dwconv/en_pwconv/en_add/en_gelu), each computed
// by its own always block from the SAME op_code source, so no single
// register fans out to multiple consumers (the actual Tier A/B
// bottleneck). Only en_add is wired to a real worker in this slice;
// the other 4 are decoded but unused stubs (TODO when their paths are
// built).
// ==============================================================
`timescale 1ns/1ps

module fastvit_top_tierb (
    input  wire         ap_clk,
    input  wire         ap_rst_n,
    output wire         interrupt,

    // ---- s_axi_control: 4 pointer base-address registers ----
    input  wire         s_axi_control_AWVALID,
    output wire         s_axi_control_AWREADY,
    input  wire [5:0]   s_axi_control_AWADDR,
    input  wire         s_axi_control_WVALID,
    output wire         s_axi_control_WREADY,
    input  wire [31:0]  s_axi_control_WDATA,
    input  wire [3:0]   s_axi_control_WSTRB,
    input  wire         s_axi_control_ARVALID,
    output wire         s_axi_control_ARREADY,
    input  wire [5:0]   s_axi_control_ARADDR,
    output wire         s_axi_control_RVALID,
    input  wire         s_axi_control_RREADY,
    output wire [31:0]  s_axi_control_RDATA,
    output wire [1:0]   s_axi_control_RRESP,
    output wire         s_axi_control_BVALID,
    input  wire         s_axi_control_BREADY,
    output wire [1:0]   s_axi_control_BRESP,

    // ---- s_axi_ctrl: op_code + 13 scalar params + ap_ctrl_hs ----
    input  wire         s_axi_ctrl_AWVALID,
    output wire         s_axi_ctrl_AWREADY,
    input  wire [6:0]   s_axi_ctrl_AWADDR,
    input  wire         s_axi_ctrl_WVALID,
    output wire         s_axi_ctrl_WREADY,
    input  wire [31:0]  s_axi_ctrl_WDATA,
    input  wire [3:0]   s_axi_ctrl_WSTRB,
    input  wire         s_axi_ctrl_ARVALID,
    output wire         s_axi_ctrl_ARREADY,
    input  wire [6:0]   s_axi_ctrl_ARADDR,
    output wire         s_axi_ctrl_RVALID,
    input  wire         s_axi_ctrl_RREADY,
    output wire [31:0]  s_axi_ctrl_RDATA,
    output wire [1:0]   s_axi_ctrl_RRESP,
    output wire         s_axi_ctrl_BVALID,
    input  wire         s_axi_ctrl_BREADY,
    output wire [1:0]   s_axi_ctrl_BRESP,

    // ---- add_worker's own private, unshared AXI4 masters ----
    // gmem0: in_a (read-only)
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    

    // gmem1: in_b (read-only)
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    

    // gmem3: out (write-only)
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    

    // ---- dwconv/pwconv/conv: each gets its own private gmem0..3 ----
    // (same read/read/read/write shape as add's gmem0/gmem1/gmem3,
    // plus gmem2 for bias)
    output wire         m_axi_dwconv_gmem0_AWVALID,
    input  wire         m_axi_dwconv_gmem0_AWREADY,
    output wire [63:0]  m_axi_dwconv_gmem0_AWADDR,
    output wire [0:0]   m_axi_dwconv_gmem0_AWID,
    output wire [7:0]   m_axi_dwconv_gmem0_AWLEN,
    output wire [2:0]   m_axi_dwconv_gmem0_AWSIZE,
    output wire [1:0]   m_axi_dwconv_gmem0_AWBURST,
    output wire [1:0]   m_axi_dwconv_gmem0_AWLOCK,
    output wire [3:0]   m_axi_dwconv_gmem0_AWCACHE,
    output wire [2:0]   m_axi_dwconv_gmem0_AWPROT,
    output wire [3:0]   m_axi_dwconv_gmem0_AWQOS,
    output wire [3:0]   m_axi_dwconv_gmem0_AWREGION,
    output wire [0:0]   m_axi_dwconv_gmem0_AWUSER,
    output wire         m_axi_dwconv_gmem0_WVALID,
    input  wire         m_axi_dwconv_gmem0_WREADY,
    output wire [31:0]  m_axi_dwconv_gmem0_WDATA,
    output wire [3:0]   m_axi_dwconv_gmem0_WSTRB,
    output wire         m_axi_dwconv_gmem0_WLAST,
    output wire [0:0]   m_axi_dwconv_gmem0_WID,
    output wire [0:0]   m_axi_dwconv_gmem0_WUSER,
    output wire         m_axi_dwconv_gmem0_ARVALID,
    input  wire         m_axi_dwconv_gmem0_ARREADY,
    output wire [63:0]  m_axi_dwconv_gmem0_ARADDR,
    output wire [0:0]   m_axi_dwconv_gmem0_ARID,
    output wire [7:0]   m_axi_dwconv_gmem0_ARLEN,
    output wire [2:0]   m_axi_dwconv_gmem0_ARSIZE,
    output wire [1:0]   m_axi_dwconv_gmem0_ARBURST,
    output wire [1:0]   m_axi_dwconv_gmem0_ARLOCK,
    output wire [3:0]   m_axi_dwconv_gmem0_ARCACHE,
    output wire [2:0]   m_axi_dwconv_gmem0_ARPROT,
    output wire [3:0]   m_axi_dwconv_gmem0_ARQOS,
    output wire [3:0]   m_axi_dwconv_gmem0_ARREGION,
    output wire [0:0]   m_axi_dwconv_gmem0_ARUSER,
    input  wire         m_axi_dwconv_gmem0_RVALID,
    output wire         m_axi_dwconv_gmem0_RREADY,
    input  wire [31:0]  m_axi_dwconv_gmem0_RDATA,
    input  wire         m_axi_dwconv_gmem0_RLAST,
    input  wire [0:0]   m_axi_dwconv_gmem0_RID,
    input  wire [0:0]   m_axi_dwconv_gmem0_RUSER,
    input  wire [1:0]   m_axi_dwconv_gmem0_RRESP,
    input  wire         m_axi_dwconv_gmem0_BVALID,
    output wire         m_axi_dwconv_gmem0_BREADY,
    input  wire [1:0]   m_axi_dwconv_gmem0_BRESP,
    input  wire [0:0]   m_axi_dwconv_gmem0_BID,
    input  wire [0:0]   m_axi_dwconv_gmem0_BUSER,
    
    // ---- shared adapters: partial-independence architecture ----
    // dwconv/pwconv keep PRIVATE gmem0(feat_in)/gmem1(weight) above --
    // those were the suspected bottleneck bundles (adapter-internal
    // FIFO cross-talk with dwconv_worker's own FSM, see project memory).
    // Everything else -- conv's all 4 bundles, add's gmem0/1/3, gelu's
    // gmem0/3, and dwconv/pwconv's own gmem2(bias)/gmem3(feat_out) --
    // now shares ONE physical adapter per gmem role (matching the
    // proven-good baseline's 4-shared-master shape), muxed by the
    // already-independent en_* dispatch registers below. 8 physical
    // masters total (4 private + 4 shared), down from 17.
    // shared_gmem0 (feat_in, read-only): conv + add + gelu
    output wire         m_axi_conv_gmem0_AWVALID,
    input  wire         m_axi_conv_gmem0_AWREADY,
    output wire [63:0]  m_axi_conv_gmem0_AWADDR,
    output wire [0:0]   m_axi_conv_gmem0_AWID,
    output wire [7:0]   m_axi_conv_gmem0_AWLEN,
    output wire [2:0]   m_axi_conv_gmem0_AWSIZE,
    output wire [1:0]   m_axi_conv_gmem0_AWBURST,
    output wire [1:0]   m_axi_conv_gmem0_AWLOCK,
    output wire [3:0]   m_axi_conv_gmem0_AWCACHE,
    output wire [2:0]   m_axi_conv_gmem0_AWPROT,
    output wire [3:0]   m_axi_conv_gmem0_AWQOS,
    output wire [3:0]   m_axi_conv_gmem0_AWREGION,
    output wire [0:0]   m_axi_conv_gmem0_AWUSER,
    output wire         m_axi_conv_gmem0_WVALID,
    input  wire         m_axi_conv_gmem0_WREADY,
    output wire [31:0]  m_axi_conv_gmem0_WDATA,
    output wire [3:0]   m_axi_conv_gmem0_WSTRB,
    output wire         m_axi_conv_gmem0_WLAST,
    output wire [0:0]   m_axi_conv_gmem0_WID,
    output wire [0:0]   m_axi_conv_gmem0_WUSER,
    output wire         m_axi_conv_gmem0_ARVALID,
    input  wire         m_axi_conv_gmem0_ARREADY,
    output wire [63:0]  m_axi_conv_gmem0_ARADDR,
    output wire [0:0]   m_axi_conv_gmem0_ARID,
    output wire [7:0]   m_axi_conv_gmem0_ARLEN,
    output wire [2:0]   m_axi_conv_gmem0_ARSIZE,
    output wire [1:0]   m_axi_conv_gmem0_ARBURST,
    output wire [1:0]   m_axi_conv_gmem0_ARLOCK,
    output wire [3:0]   m_axi_conv_gmem0_ARCACHE,
    output wire [2:0]   m_axi_conv_gmem0_ARPROT,
    output wire [3:0]   m_axi_conv_gmem0_ARQOS,
    output wire [3:0]   m_axi_conv_gmem0_ARREGION,
    output wire [0:0]   m_axi_conv_gmem0_ARUSER,
    input  wire         m_axi_conv_gmem0_RVALID,
    output wire         m_axi_conv_gmem0_RREADY,
    input  wire [31:0]  m_axi_conv_gmem0_RDATA,
    input  wire         m_axi_conv_gmem0_RLAST,
    input  wire [0:0]   m_axi_conv_gmem0_RID,
    input  wire [0:0]   m_axi_conv_gmem0_RUSER,
    input  wire [1:0]   m_axi_conv_gmem0_RRESP,
    input  wire         m_axi_conv_gmem0_BVALID,
    output wire         m_axi_conv_gmem0_BREADY,
    input  wire [1:0]   m_axi_conv_gmem0_BRESP,
    input  wire [0:0]   m_axi_conv_gmem0_BID,
    input  wire [0:0]   m_axi_conv_gmem0_BUSER,
output wire         m_axi_add_gmem0_AWVALID,
    input  wire         m_axi_add_gmem0_AWREADY,
    output wire [63:0]  m_axi_add_gmem0_AWADDR,
    output wire [0:0]   m_axi_add_gmem0_AWID,
    output wire [7:0]   m_axi_add_gmem0_AWLEN,
    output wire [2:0]   m_axi_add_gmem0_AWSIZE,
    output wire [1:0]   m_axi_add_gmem0_AWBURST,
    output wire [1:0]   m_axi_add_gmem0_AWLOCK,
    output wire [3:0]   m_axi_add_gmem0_AWCACHE,
    output wire [2:0]   m_axi_add_gmem0_AWPROT,
    output wire [3:0]   m_axi_add_gmem0_AWQOS,
    output wire [3:0]   m_axi_add_gmem0_AWREGION,
    output wire [0:0]   m_axi_add_gmem0_AWUSER,
    output wire         m_axi_add_gmem0_WVALID,
    input  wire         m_axi_add_gmem0_WREADY,
    output wire [31:0]  m_axi_add_gmem0_WDATA,
    output wire [3:0]   m_axi_add_gmem0_WSTRB,
    output wire         m_axi_add_gmem0_WLAST,
    output wire [0:0]   m_axi_add_gmem0_WID,
    output wire [0:0]   m_axi_add_gmem0_WUSER,
    output wire         m_axi_add_gmem0_ARVALID,
    input  wire         m_axi_add_gmem0_ARREADY,
    output wire [63:0]  m_axi_add_gmem0_ARADDR,
    output wire [0:0]   m_axi_add_gmem0_ARID,
    output wire [7:0]   m_axi_add_gmem0_ARLEN,
    output wire [2:0]   m_axi_add_gmem0_ARSIZE,
    output wire [1:0]   m_axi_add_gmem0_ARBURST,
    output wire [1:0]   m_axi_add_gmem0_ARLOCK,
    output wire [3:0]   m_axi_add_gmem0_ARCACHE,
    output wire [2:0]   m_axi_add_gmem0_ARPROT,
    output wire [3:0]   m_axi_add_gmem0_ARQOS,
    output wire [3:0]   m_axi_add_gmem0_ARREGION,
    output wire [0:0]   m_axi_add_gmem0_ARUSER,
    input  wire         m_axi_add_gmem0_RVALID,
    output wire         m_axi_add_gmem0_RREADY,
    input  wire [31:0]  m_axi_add_gmem0_RDATA,
    input  wire         m_axi_add_gmem0_RLAST,
    input  wire [0:0]   m_axi_add_gmem0_RID,
    input  wire [0:0]   m_axi_add_gmem0_RUSER,
    input  wire [1:0]   m_axi_add_gmem0_RRESP,
    input  wire         m_axi_add_gmem0_BVALID,
    output wire         m_axi_add_gmem0_BREADY,
    input  wire [1:0]   m_axi_add_gmem0_BRESP,
    input  wire [0:0]   m_axi_add_gmem0_BID,
    input  wire [0:0]   m_axi_add_gmem0_BUSER,
output wire         m_axi_gelu_gmem0_AWVALID,
    input  wire         m_axi_gelu_gmem0_AWREADY,
    output wire [63:0]  m_axi_gelu_gmem0_AWADDR,
    output wire [0:0]   m_axi_gelu_gmem0_AWID,
    output wire [7:0]   m_axi_gelu_gmem0_AWLEN,
    output wire [2:0]   m_axi_gelu_gmem0_AWSIZE,
    output wire [1:0]   m_axi_gelu_gmem0_AWBURST,
    output wire [1:0]   m_axi_gelu_gmem0_AWLOCK,
    output wire [3:0]   m_axi_gelu_gmem0_AWCACHE,
    output wire [2:0]   m_axi_gelu_gmem0_AWPROT,
    output wire [3:0]   m_axi_gelu_gmem0_AWQOS,
    output wire [3:0]   m_axi_gelu_gmem0_AWREGION,
    output wire [0:0]   m_axi_gelu_gmem0_AWUSER,
    output wire         m_axi_gelu_gmem0_WVALID,
    input  wire         m_axi_gelu_gmem0_WREADY,
    output wire [31:0]  m_axi_gelu_gmem0_WDATA,
    output wire [3:0]   m_axi_gelu_gmem0_WSTRB,
    output wire         m_axi_gelu_gmem0_WLAST,
    output wire [0:0]   m_axi_gelu_gmem0_WID,
    output wire [0:0]   m_axi_gelu_gmem0_WUSER,
    output wire         m_axi_gelu_gmem0_ARVALID,
    input  wire         m_axi_gelu_gmem0_ARREADY,
    output wire [63:0]  m_axi_gelu_gmem0_ARADDR,
    output wire [0:0]   m_axi_gelu_gmem0_ARID,
    output wire [7:0]   m_axi_gelu_gmem0_ARLEN,
    output wire [2:0]   m_axi_gelu_gmem0_ARSIZE,
    output wire [1:0]   m_axi_gelu_gmem0_ARBURST,
    output wire [1:0]   m_axi_gelu_gmem0_ARLOCK,
    output wire [3:0]   m_axi_gelu_gmem0_ARCACHE,
    output wire [2:0]   m_axi_gelu_gmem0_ARPROT,
    output wire [3:0]   m_axi_gelu_gmem0_ARQOS,
    output wire [3:0]   m_axi_gelu_gmem0_ARREGION,
    output wire [0:0]   m_axi_gelu_gmem0_ARUSER,
    input  wire         m_axi_gelu_gmem0_RVALID,
    output wire         m_axi_gelu_gmem0_RREADY,
    input  wire [31:0]  m_axi_gelu_gmem0_RDATA,
    input  wire         m_axi_gelu_gmem0_RLAST,
    input  wire [0:0]   m_axi_gelu_gmem0_RID,
    input  wire [0:0]   m_axi_gelu_gmem0_RUSER,
    input  wire [1:0]   m_axi_gelu_gmem0_RRESP,
    input  wire         m_axi_gelu_gmem0_BVALID,
    output wire         m_axi_gelu_gmem0_BREADY,
    input  wire [1:0]   m_axi_gelu_gmem0_BRESP,
    input  wire [0:0]   m_axi_gelu_gmem0_BID,
    input  wire [0:0]   m_axi_gelu_gmem0_BUSER,
output wire         m_axi_conv_gmem3_AWVALID,
    input  wire         m_axi_conv_gmem3_AWREADY,
    output wire [63:0]  m_axi_conv_gmem3_AWADDR,
    output wire [0:0]   m_axi_conv_gmem3_AWID,
    output wire [7:0]   m_axi_conv_gmem3_AWLEN,
    output wire [2:0]   m_axi_conv_gmem3_AWSIZE,
    output wire [1:0]   m_axi_conv_gmem3_AWBURST,
    output wire [1:0]   m_axi_conv_gmem3_AWLOCK,
    output wire [3:0]   m_axi_conv_gmem3_AWCACHE,
    output wire [2:0]   m_axi_conv_gmem3_AWPROT,
    output wire [3:0]   m_axi_conv_gmem3_AWQOS,
    output wire [3:0]   m_axi_conv_gmem3_AWREGION,
    output wire [0:0]   m_axi_conv_gmem3_AWUSER,
    output wire         m_axi_conv_gmem3_WVALID,
    input  wire         m_axi_conv_gmem3_WREADY,
    output wire [31:0]  m_axi_conv_gmem3_WDATA,
    output wire [3:0]   m_axi_conv_gmem3_WSTRB,
    output wire         m_axi_conv_gmem3_WLAST,
    output wire [0:0]   m_axi_conv_gmem3_WID,
    output wire [0:0]   m_axi_conv_gmem3_WUSER,
    output wire         m_axi_conv_gmem3_ARVALID,
    input  wire         m_axi_conv_gmem3_ARREADY,
    output wire [63:0]  m_axi_conv_gmem3_ARADDR,
    output wire [0:0]   m_axi_conv_gmem3_ARID,
    output wire [7:0]   m_axi_conv_gmem3_ARLEN,
    output wire [2:0]   m_axi_conv_gmem3_ARSIZE,
    output wire [1:0]   m_axi_conv_gmem3_ARBURST,
    output wire [1:0]   m_axi_conv_gmem3_ARLOCK,
    output wire [3:0]   m_axi_conv_gmem3_ARCACHE,
    output wire [2:0]   m_axi_conv_gmem3_ARPROT,
    output wire [3:0]   m_axi_conv_gmem3_ARQOS,
    output wire [3:0]   m_axi_conv_gmem3_ARREGION,
    output wire [0:0]   m_axi_conv_gmem3_ARUSER,
    input  wire         m_axi_conv_gmem3_RVALID,
    output wire         m_axi_conv_gmem3_RREADY,
    input  wire [31:0]  m_axi_conv_gmem3_RDATA,
    input  wire         m_axi_conv_gmem3_RLAST,
    input  wire [0:0]   m_axi_conv_gmem3_RID,
    input  wire [0:0]   m_axi_conv_gmem3_RUSER,
    input  wire [1:0]   m_axi_conv_gmem3_RRESP,
    input  wire         m_axi_conv_gmem3_BVALID,
    output wire         m_axi_conv_gmem3_BREADY,
    input  wire [1:0]   m_axi_conv_gmem3_BRESP,
    input  wire [0:0]   m_axi_conv_gmem3_BID,
    input  wire [0:0]   m_axi_conv_gmem3_BUSER,
output wire         m_axi_dwconv_gmem3_AWVALID,
    input  wire         m_axi_dwconv_gmem3_AWREADY,
    output wire [63:0]  m_axi_dwconv_gmem3_AWADDR,
    output wire [0:0]   m_axi_dwconv_gmem3_AWID,
    output wire [7:0]   m_axi_dwconv_gmem3_AWLEN,
    output wire [2:0]   m_axi_dwconv_gmem3_AWSIZE,
    output wire [1:0]   m_axi_dwconv_gmem3_AWBURST,
    output wire [1:0]   m_axi_dwconv_gmem3_AWLOCK,
    output wire [3:0]   m_axi_dwconv_gmem3_AWCACHE,
    output wire [2:0]   m_axi_dwconv_gmem3_AWPROT,
    output wire [3:0]   m_axi_dwconv_gmem3_AWQOS,
    output wire [3:0]   m_axi_dwconv_gmem3_AWREGION,
    output wire [0:0]   m_axi_dwconv_gmem3_AWUSER,
    output wire         m_axi_dwconv_gmem3_WVALID,
    input  wire         m_axi_dwconv_gmem3_WREADY,
    output wire [31:0]  m_axi_dwconv_gmem3_WDATA,
    output wire [3:0]   m_axi_dwconv_gmem3_WSTRB,
    output wire         m_axi_dwconv_gmem3_WLAST,
    output wire [0:0]   m_axi_dwconv_gmem3_WID,
    output wire [0:0]   m_axi_dwconv_gmem3_WUSER,
    output wire         m_axi_dwconv_gmem3_ARVALID,
    input  wire         m_axi_dwconv_gmem3_ARREADY,
    output wire [63:0]  m_axi_dwconv_gmem3_ARADDR,
    output wire [0:0]   m_axi_dwconv_gmem3_ARID,
    output wire [7:0]   m_axi_dwconv_gmem3_ARLEN,
    output wire [2:0]   m_axi_dwconv_gmem3_ARSIZE,
    output wire [1:0]   m_axi_dwconv_gmem3_ARBURST,
    output wire [1:0]   m_axi_dwconv_gmem3_ARLOCK,
    output wire [3:0]   m_axi_dwconv_gmem3_ARCACHE,
    output wire [2:0]   m_axi_dwconv_gmem3_ARPROT,
    output wire [3:0]   m_axi_dwconv_gmem3_ARQOS,
    output wire [3:0]   m_axi_dwconv_gmem3_ARREGION,
    output wire [0:0]   m_axi_dwconv_gmem3_ARUSER,
    input  wire         m_axi_dwconv_gmem3_RVALID,
    output wire         m_axi_dwconv_gmem3_RREADY,
    input  wire [31:0]  m_axi_dwconv_gmem3_RDATA,
    input  wire         m_axi_dwconv_gmem3_RLAST,
    input  wire [0:0]   m_axi_dwconv_gmem3_RID,
    input  wire [0:0]   m_axi_dwconv_gmem3_RUSER,
    input  wire [1:0]   m_axi_dwconv_gmem3_RRESP,
    input  wire         m_axi_dwconv_gmem3_BVALID,
    output wire         m_axi_dwconv_gmem3_BREADY,
    input  wire [1:0]   m_axi_dwconv_gmem3_BRESP,
    input  wire [0:0]   m_axi_dwconv_gmem3_BID,
    input  wire [0:0]   m_axi_dwconv_gmem3_BUSER,
output wire         m_axi_pwconv_gmem3_AWVALID,
    input  wire         m_axi_pwconv_gmem3_AWREADY,
    output wire [63:0]  m_axi_pwconv_gmem3_AWADDR,
    output wire [0:0]   m_axi_pwconv_gmem3_AWID,
    output wire [7:0]   m_axi_pwconv_gmem3_AWLEN,
    output wire [2:0]   m_axi_pwconv_gmem3_AWSIZE,
    output wire [1:0]   m_axi_pwconv_gmem3_AWBURST,
    output wire [1:0]   m_axi_pwconv_gmem3_AWLOCK,
    output wire [3:0]   m_axi_pwconv_gmem3_AWCACHE,
    output wire [2:0]   m_axi_pwconv_gmem3_AWPROT,
    output wire [3:0]   m_axi_pwconv_gmem3_AWQOS,
    output wire [3:0]   m_axi_pwconv_gmem3_AWREGION,
    output wire [0:0]   m_axi_pwconv_gmem3_AWUSER,
    output wire         m_axi_pwconv_gmem3_WVALID,
    input  wire         m_axi_pwconv_gmem3_WREADY,
    output wire [31:0]  m_axi_pwconv_gmem3_WDATA,
    output wire [3:0]   m_axi_pwconv_gmem3_WSTRB,
    output wire         m_axi_pwconv_gmem3_WLAST,
    output wire [0:0]   m_axi_pwconv_gmem3_WID,
    output wire [0:0]   m_axi_pwconv_gmem3_WUSER,
    output wire         m_axi_pwconv_gmem3_ARVALID,
    input  wire         m_axi_pwconv_gmem3_ARREADY,
    output wire [63:0]  m_axi_pwconv_gmem3_ARADDR,
    output wire [0:0]   m_axi_pwconv_gmem3_ARID,
    output wire [7:0]   m_axi_pwconv_gmem3_ARLEN,
    output wire [2:0]   m_axi_pwconv_gmem3_ARSIZE,
    output wire [1:0]   m_axi_pwconv_gmem3_ARBURST,
    output wire [1:0]   m_axi_pwconv_gmem3_ARLOCK,
    output wire [3:0]   m_axi_pwconv_gmem3_ARCACHE,
    output wire [2:0]   m_axi_pwconv_gmem3_ARPROT,
    output wire [3:0]   m_axi_pwconv_gmem3_ARQOS,
    output wire [3:0]   m_axi_pwconv_gmem3_ARREGION,
    output wire [0:0]   m_axi_pwconv_gmem3_ARUSER,
    input  wire         m_axi_pwconv_gmem3_RVALID,
    output wire         m_axi_pwconv_gmem3_RREADY,
    input  wire [31:0]  m_axi_pwconv_gmem3_RDATA,
    input  wire         m_axi_pwconv_gmem3_RLAST,
    input  wire [0:0]   m_axi_pwconv_gmem3_RID,
    input  wire [0:0]   m_axi_pwconv_gmem3_RUSER,
    input  wire [1:0]   m_axi_pwconv_gmem3_RRESP,
    input  wire         m_axi_pwconv_gmem3_BVALID,
    output wire         m_axi_pwconv_gmem3_BREADY,
    input  wire [1:0]   m_axi_pwconv_gmem3_BRESP,
    input  wire [0:0]   m_axi_pwconv_gmem3_BID,
    input  wire [0:0]   m_axi_pwconv_gmem3_BUSER,
output wire         m_axi_add_gmem3_AWVALID,
    input  wire         m_axi_add_gmem3_AWREADY,
    output wire [63:0]  m_axi_add_gmem3_AWADDR,
    output wire [0:0]   m_axi_add_gmem3_AWID,
    output wire [7:0]   m_axi_add_gmem3_AWLEN,
    output wire [2:0]   m_axi_add_gmem3_AWSIZE,
    output wire [1:0]   m_axi_add_gmem3_AWBURST,
    output wire [1:0]   m_axi_add_gmem3_AWLOCK,
    output wire [3:0]   m_axi_add_gmem3_AWCACHE,
    output wire [2:0]   m_axi_add_gmem3_AWPROT,
    output wire [3:0]   m_axi_add_gmem3_AWQOS,
    output wire [3:0]   m_axi_add_gmem3_AWREGION,
    output wire [0:0]   m_axi_add_gmem3_AWUSER,
    output wire         m_axi_add_gmem3_WVALID,
    input  wire         m_axi_add_gmem3_WREADY,
    output wire [31:0]  m_axi_add_gmem3_WDATA,
    output wire [3:0]   m_axi_add_gmem3_WSTRB,
    output wire         m_axi_add_gmem3_WLAST,
    output wire [0:0]   m_axi_add_gmem3_WID,
    output wire [0:0]   m_axi_add_gmem3_WUSER,
    output wire         m_axi_add_gmem3_ARVALID,
    input  wire         m_axi_add_gmem3_ARREADY,
    output wire [63:0]  m_axi_add_gmem3_ARADDR,
    output wire [0:0]   m_axi_add_gmem3_ARID,
    output wire [7:0]   m_axi_add_gmem3_ARLEN,
    output wire [2:0]   m_axi_add_gmem3_ARSIZE,
    output wire [1:0]   m_axi_add_gmem3_ARBURST,
    output wire [1:0]   m_axi_add_gmem3_ARLOCK,
    output wire [3:0]   m_axi_add_gmem3_ARCACHE,
    output wire [2:0]   m_axi_add_gmem3_ARPROT,
    output wire [3:0]   m_axi_add_gmem3_ARQOS,
    output wire [3:0]   m_axi_add_gmem3_ARREGION,
    output wire [0:0]   m_axi_add_gmem3_ARUSER,
    input  wire         m_axi_add_gmem3_RVALID,
    output wire         m_axi_add_gmem3_RREADY,
    input  wire [31:0]  m_axi_add_gmem3_RDATA,
    input  wire         m_axi_add_gmem3_RLAST,
    input  wire [0:0]   m_axi_add_gmem3_RID,
    input  wire [0:0]   m_axi_add_gmem3_RUSER,
    input  wire [1:0]   m_axi_add_gmem3_RRESP,
    input  wire         m_axi_add_gmem3_BVALID,
    output wire         m_axi_add_gmem3_BREADY,
    input  wire [1:0]   m_axi_add_gmem3_BRESP,
    input  wire [0:0]   m_axi_add_gmem3_BID,
    input  wire [0:0]   m_axi_add_gmem3_BUSER,
output wire         m_axi_gelu_gmem3_AWVALID,
    input  wire         m_axi_gelu_gmem3_AWREADY,
    output wire [63:0]  m_axi_gelu_gmem3_AWADDR,
    output wire [0:0]   m_axi_gelu_gmem3_AWID,
    output wire [7:0]   m_axi_gelu_gmem3_AWLEN,
    output wire [2:0]   m_axi_gelu_gmem3_AWSIZE,
    output wire [1:0]   m_axi_gelu_gmem3_AWBURST,
    output wire [1:0]   m_axi_gelu_gmem3_AWLOCK,
    output wire [3:0]   m_axi_gelu_gmem3_AWCACHE,
    output wire [2:0]   m_axi_gelu_gmem3_AWPROT,
    output wire [3:0]   m_axi_gelu_gmem3_AWQOS,
    output wire [3:0]   m_axi_gelu_gmem3_AWREGION,
    output wire [0:0]   m_axi_gelu_gmem3_AWUSER,
    output wire         m_axi_gelu_gmem3_WVALID,
    input  wire         m_axi_gelu_gmem3_WREADY,
    output wire [31:0]  m_axi_gelu_gmem3_WDATA,
    output wire [3:0]   m_axi_gelu_gmem3_WSTRB,
    output wire         m_axi_gelu_gmem3_WLAST,
    output wire [0:0]   m_axi_gelu_gmem3_WID,
    output wire [0:0]   m_axi_gelu_gmem3_WUSER,
    output wire         m_axi_gelu_gmem3_ARVALID,
    input  wire         m_axi_gelu_gmem3_ARREADY,
    output wire [63:0]  m_axi_gelu_gmem3_ARADDR,
    output wire [0:0]   m_axi_gelu_gmem3_ARID,
    output wire [7:0]   m_axi_gelu_gmem3_ARLEN,
    output wire [2:0]   m_axi_gelu_gmem3_ARSIZE,
    output wire [1:0]   m_axi_gelu_gmem3_ARBURST,
    output wire [1:0]   m_axi_gelu_gmem3_ARLOCK,
    output wire [3:0]   m_axi_gelu_gmem3_ARCACHE,
    output wire [2:0]   m_axi_gelu_gmem3_ARPROT,
    output wire [3:0]   m_axi_gelu_gmem3_ARQOS,
    output wire [3:0]   m_axi_gelu_gmem3_ARREGION,
    output wire [0:0]   m_axi_gelu_gmem3_ARUSER,
    input  wire         m_axi_gelu_gmem3_RVALID,
    output wire         m_axi_gelu_gmem3_RREADY,
    input  wire [31:0]  m_axi_gelu_gmem3_RDATA,
    input  wire         m_axi_gelu_gmem3_RLAST,
    input  wire [0:0]   m_axi_gelu_gmem3_RID,
    input  wire [0:0]   m_axi_gelu_gmem3_RUSER,
    input  wire [1:0]   m_axi_gelu_gmem3_RRESP,
    input  wire         m_axi_gelu_gmem3_BVALID,
    output wire         m_axi_gelu_gmem3_BREADY,
    input  wire [1:0]   m_axi_gelu_gmem3_BRESP,
    input  wire [0:0]   m_axi_gelu_gmem3_BID,
    input  wire [0:0]   m_axi_gelu_gmem3_BUSER,

    // shared_gmem1 (weight/in_b, read-only): conv + add
    output wire         m_axi_shared_gmem1_AWVALID,
    input  wire         m_axi_shared_gmem1_AWREADY,
    output wire [63:0]  m_axi_shared_gmem1_AWADDR,
    output wire [0:0]   m_axi_shared_gmem1_AWID,
    output wire [7:0]   m_axi_shared_gmem1_AWLEN,
    output wire [2:0]   m_axi_shared_gmem1_AWSIZE,
    output wire [1:0]   m_axi_shared_gmem1_AWBURST,
    output wire [1:0]   m_axi_shared_gmem1_AWLOCK,
    output wire [3:0]   m_axi_shared_gmem1_AWCACHE,
    output wire [2:0]   m_axi_shared_gmem1_AWPROT,
    output wire [3:0]   m_axi_shared_gmem1_AWQOS,
    output wire [3:0]   m_axi_shared_gmem1_AWREGION,
    output wire [0:0]   m_axi_shared_gmem1_AWUSER,
    output wire         m_axi_shared_gmem1_WVALID,
    input  wire         m_axi_shared_gmem1_WREADY,
    output wire [31:0]  m_axi_shared_gmem1_WDATA,
    output wire [3:0]   m_axi_shared_gmem1_WSTRB,
    output wire         m_axi_shared_gmem1_WLAST,
    output wire [0:0]   m_axi_shared_gmem1_WID,
    output wire [0:0]   m_axi_shared_gmem1_WUSER,
    output wire         m_axi_shared_gmem1_ARVALID,
    input  wire         m_axi_shared_gmem1_ARREADY,
    output wire [63:0]  m_axi_shared_gmem1_ARADDR,
    output wire [0:0]   m_axi_shared_gmem1_ARID,
    output wire [7:0]   m_axi_shared_gmem1_ARLEN,
    output wire [2:0]   m_axi_shared_gmem1_ARSIZE,
    output wire [1:0]   m_axi_shared_gmem1_ARBURST,
    output wire [1:0]   m_axi_shared_gmem1_ARLOCK,
    output wire [3:0]   m_axi_shared_gmem1_ARCACHE,
    output wire [2:0]   m_axi_shared_gmem1_ARPROT,
    output wire [3:0]   m_axi_shared_gmem1_ARQOS,
    output wire [3:0]   m_axi_shared_gmem1_ARREGION,
    output wire [0:0]   m_axi_shared_gmem1_ARUSER,
    input  wire         m_axi_shared_gmem1_RVALID,
    output wire         m_axi_shared_gmem1_RREADY,
    input  wire [31:0]  m_axi_shared_gmem1_RDATA,
    input  wire         m_axi_shared_gmem1_RLAST,
    input  wire [0:0]   m_axi_shared_gmem1_RID,
    input  wire [0:0]   m_axi_shared_gmem1_RUSER,
    input  wire [1:0]   m_axi_shared_gmem1_RRESP,
    input  wire         m_axi_shared_gmem1_BVALID,
    output wire         m_axi_shared_gmem1_BREADY,
    input  wire [1:0]   m_axi_shared_gmem1_BRESP,
    input  wire [0:0]   m_axi_shared_gmem1_BID,
    input  wire [0:0]   m_axi_shared_gmem1_BUSER,
    // shared_gmem2 (bias, read-only): conv + dwconv + pwconv
    output wire         m_axi_shared_gmem2_AWVALID,
    input  wire         m_axi_shared_gmem2_AWREADY,
    output wire [63:0]  m_axi_shared_gmem2_AWADDR,
    output wire [0:0]   m_axi_shared_gmem2_AWID,
    output wire [7:0]   m_axi_shared_gmem2_AWLEN,
    output wire [2:0]   m_axi_shared_gmem2_AWSIZE,
    output wire [1:0]   m_axi_shared_gmem2_AWBURST,
    output wire [1:0]   m_axi_shared_gmem2_AWLOCK,
    output wire [3:0]   m_axi_shared_gmem2_AWCACHE,
    output wire [2:0]   m_axi_shared_gmem2_AWPROT,
    output wire [3:0]   m_axi_shared_gmem2_AWQOS,
    output wire [3:0]   m_axi_shared_gmem2_AWREGION,
    output wire [0:0]   m_axi_shared_gmem2_AWUSER,
    output wire         m_axi_shared_gmem2_WVALID,
    input  wire         m_axi_shared_gmem2_WREADY,
    output wire [31:0]  m_axi_shared_gmem2_WDATA,
    output wire [3:0]   m_axi_shared_gmem2_WSTRB,
    output wire         m_axi_shared_gmem2_WLAST,
    output wire [0:0]   m_axi_shared_gmem2_WID,
    output wire [0:0]   m_axi_shared_gmem2_WUSER,
    output wire         m_axi_shared_gmem2_ARVALID,
    input  wire         m_axi_shared_gmem2_ARREADY,
    output wire [63:0]  m_axi_shared_gmem2_ARADDR,
    output wire [0:0]   m_axi_shared_gmem2_ARID,
    output wire [7:0]   m_axi_shared_gmem2_ARLEN,
    output wire [2:0]   m_axi_shared_gmem2_ARSIZE,
    output wire [1:0]   m_axi_shared_gmem2_ARBURST,
    output wire [1:0]   m_axi_shared_gmem2_ARLOCK,
    output wire [3:0]   m_axi_shared_gmem2_ARCACHE,
    output wire [2:0]   m_axi_shared_gmem2_ARPROT,
    output wire [3:0]   m_axi_shared_gmem2_ARQOS,
    output wire [3:0]   m_axi_shared_gmem2_ARREGION,
    output wire [0:0]   m_axi_shared_gmem2_ARUSER,
    input  wire         m_axi_shared_gmem2_RVALID,
    output wire         m_axi_shared_gmem2_RREADY,
    input  wire [31:0]  m_axi_shared_gmem2_RDATA,
    input  wire         m_axi_shared_gmem2_RLAST,
    input  wire [0:0]   m_axi_shared_gmem2_RID,
    input  wire [0:0]   m_axi_shared_gmem2_RUSER,
    input  wire [1:0]   m_axi_shared_gmem2_RRESP,
    input  wire         m_axi_shared_gmem2_BVALID,
    output wire         m_axi_shared_gmem2_BREADY,
    input  wire [1:0]   m_axi_shared_gmem2_BRESP,
    input  wire [0:0]   m_axi_shared_gmem2_BID,
    input  wire [0:0]   m_axi_shared_gmem2_BUSER,
    // shared_gmem3 (feat_out, write-only): conv + add + gelu + dwconv + pwconv
    

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
        output wire         m_axi_pwconv_gmem0_AWVALID,
    input  wire         m_axi_pwconv_gmem0_AWREADY,
    output wire [63:0]  m_axi_pwconv_gmem0_AWADDR,
    output wire [0:0]   m_axi_pwconv_gmem0_AWID,
    output wire [7:0]   m_axi_pwconv_gmem0_AWLEN,
    output wire [2:0]   m_axi_pwconv_gmem0_AWSIZE,
    output wire [1:0]   m_axi_pwconv_gmem0_AWBURST,
    output wire [1:0]   m_axi_pwconv_gmem0_AWLOCK,
    output wire [3:0]   m_axi_pwconv_gmem0_AWCACHE,
    output wire [2:0]   m_axi_pwconv_gmem0_AWPROT,
    output wire [3:0]   m_axi_pwconv_gmem0_AWQOS,
    output wire [3:0]   m_axi_pwconv_gmem0_AWREGION,
    output wire [0:0]   m_axi_pwconv_gmem0_AWUSER,
    output wire         m_axi_pwconv_gmem0_WVALID,
    input  wire         m_axi_pwconv_gmem0_WREADY,
    output wire [31:0]  m_axi_pwconv_gmem0_WDATA,
    output wire [3:0]   m_axi_pwconv_gmem0_WSTRB,
    output wire         m_axi_pwconv_gmem0_WLAST,
    output wire [0:0]   m_axi_pwconv_gmem0_WID,
    output wire [0:0]   m_axi_pwconv_gmem0_WUSER,
    output wire         m_axi_pwconv_gmem0_ARVALID,
    input  wire         m_axi_pwconv_gmem0_ARREADY,
    output wire [63:0]  m_axi_pwconv_gmem0_ARADDR,
    output wire [0:0]   m_axi_pwconv_gmem0_ARID,
    output wire [7:0]   m_axi_pwconv_gmem0_ARLEN,
    output wire [2:0]   m_axi_pwconv_gmem0_ARSIZE,
    output wire [1:0]   m_axi_pwconv_gmem0_ARBURST,
    output wire [1:0]   m_axi_pwconv_gmem0_ARLOCK,
    output wire [3:0]   m_axi_pwconv_gmem0_ARCACHE,
    output wire [2:0]   m_axi_pwconv_gmem0_ARPROT,
    output wire [3:0]   m_axi_pwconv_gmem0_ARQOS,
    output wire [3:0]   m_axi_pwconv_gmem0_ARREGION,
    output wire [0:0]   m_axi_pwconv_gmem0_ARUSER,
    input  wire         m_axi_pwconv_gmem0_RVALID,
    output wire         m_axi_pwconv_gmem0_RREADY,
    input  wire [31:0]  m_axi_pwconv_gmem0_RDATA,
    input  wire         m_axi_pwconv_gmem0_RLAST,
    input  wire [0:0]   m_axi_pwconv_gmem0_RID,
    input  wire [0:0]   m_axi_pwconv_gmem0_RUSER,
    input  wire [1:0]   m_axi_pwconv_gmem0_RRESP,
    input  wire         m_axi_pwconv_gmem0_BVALID,
    output wire         m_axi_pwconv_gmem0_BREADY,
    input  wire [1:0]   m_axi_pwconv_gmem0_BRESP,
    input  wire [0:0]   m_axi_pwconv_gmem0_BID,
    input  wire [0:0]   m_axi_pwconv_gmem0_BUSER,
    

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
        
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    

    // ---- gelu: hand-written FSM, own private gmem0(read)/gmem3(write) ----
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    );

localparam OP_CONV   = 32'd0;
localparam OP_DWCONV = 32'd1;
localparam OP_PWCONV = 32'd2;
localparam OP_ADD    = 32'd3;
localparam OP_GELU   = 32'd4;

wire ap_rst_n_inv = ~ap_rst_n;

// ------------------------------------------------------------------
// AXI4-Lite register files (unmodified HLS-generated black boxes)
// ------------------------------------------------------------------
wire [63:0] in_a_addr, in_b_addr, bias_addr, out_r_addr;

fastvit_ip_control_s_axi #(.C_S_AXI_ADDR_WIDTH(6), .C_S_AXI_DATA_WIDTH(32))
control_s_axi_U (
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .AWADDR(s_axi_control_AWADDR), .AWVALID(s_axi_control_AWVALID), .AWREADY(s_axi_control_AWREADY),
    .WDATA(s_axi_control_WDATA), .WSTRB(s_axi_control_WSTRB), .WVALID(s_axi_control_WVALID), .WREADY(s_axi_control_WREADY),
    .BRESP(s_axi_control_BRESP), .BVALID(s_axi_control_BVALID), .BREADY(s_axi_control_BREADY),
    .ARADDR(s_axi_control_ARADDR), .ARVALID(s_axi_control_ARVALID), .ARREADY(s_axi_control_ARREADY),
    .RDATA(s_axi_control_RDATA), .RRESP(s_axi_control_RRESP), .RVALID(s_axi_control_RVALID), .RREADY(s_axi_control_RREADY),
    .in_a(in_a_addr), .in_b(in_b_addr), .bias(bias_addr), .out_r(out_r_addr)
);

wire [31:0] op_code_w, CHin_w, Hin_w, Win_w, CHout_w, act_mode_w, out_shift_w;
wire [31:0] stride_h_w, stride_w_w, pad_h_w, pad_w_w, Kh_w, Kw_w, fpg_w;
wire        ap_start_lite;
wire        ap_done_o, ap_ready_o, ap_idle_o;

fastvit_ip_ctrl_s_axi #(.C_S_AXI_ADDR_WIDTH(7), .C_S_AXI_DATA_WIDTH(32))
ctrl_s_axi_U (
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .AWADDR(s_axi_ctrl_AWADDR), .AWVALID(s_axi_ctrl_AWVALID), .AWREADY(s_axi_ctrl_AWREADY),
    .WDATA(s_axi_ctrl_WDATA), .WSTRB(s_axi_ctrl_WSTRB), .WVALID(s_axi_ctrl_WVALID), .WREADY(s_axi_ctrl_WREADY),
    .BRESP(s_axi_ctrl_BRESP), .BVALID(s_axi_ctrl_BVALID), .BREADY(s_axi_ctrl_BREADY),
    .ARADDR(s_axi_ctrl_ARADDR), .ARVALID(s_axi_ctrl_ARVALID), .ARREADY(s_axi_ctrl_ARREADY),
    .RDATA(s_axi_ctrl_RDATA), .RRESP(s_axi_ctrl_RRESP), .RVALID(s_axi_ctrl_RVALID), .RREADY(s_axi_ctrl_RREADY),
    .interrupt(interrupt),
    .op_code(op_code_w), .CHin(CHin_w), .Hin(Hin_w), .Win(Win_w), .CHout(CHout_w),
    .act_mode(act_mode_w), .out_shift(out_shift_w),
    .stride_h(stride_h_w), .stride_w(stride_w_w), .pad_h(pad_h_w), .pad_w(pad_w_w),
    .Kh(Kh_w), .Kw(Kw_w), .fpg(fpg_w),
    .ap_start(ap_start_lite), .ap_done(ap_done_o), .ap_ready(ap_ready_o), .ap_idle(ap_idle_o)
);

// ------------------------------------------------------------------
// op_code dispatch: one independently-registered enable per op,
// each (* dont_touch *) so synthesis can't re-merge them into a
// single shared high-fanout decode net (the proven Tier A bottleneck).
// Only en_add is wired to a real worker in this slice.
// ------------------------------------------------------------------
reg busy_r;
(* dont_touch = "true" *) reg en_conv_r;
(* dont_touch = "true" *) reg en_dwconv_r;
(* dont_touch = "true" *) reg en_pwconv_r;
(* dont_touch = "true" *) reg en_add_r;
(* dont_touch = "true" *) reg en_gelu_r;

wire add_worker_ap_done, add_worker_ap_idle, add_worker_ap_ready;

// Declared here (used by op_done below); driven further down alongside
// each worker's instantiation.
wire conv_worker_ap_done, conv_worker_ap_idle, conv_worker_ap_ready;
wire dwconv_worker_ap_done, dwconv_worker_ap_idle, dwconv_worker_ap_ready;
wire pwconv_worker_ap_done, pwconv_worker_ap_idle, pwconv_worker_ap_ready;
wire gelu_ap_done, gelu_ap_idle;

wire op_done = (en_conv_r   & conv_worker_ap_done)
             | (en_dwconv_r & dwconv_worker_ap_done)
             | (en_pwconv_r & pwconv_worker_ap_done)
             | (en_add_r    & add_worker_ap_done)
             | (en_gelu_r   & gelu_ap_done);

always @(posedge ap_clk) begin
    if (ap_rst_n_inv) busy_r <= 1'b0;
    else if (!busy_r && ap_start_lite) busy_r <= 1'b1;
    else if (busy_r && op_done)        busy_r <= 1'b0;
end

always @(posedge ap_clk) begin
    if (ap_rst_n_inv)                       en_conv_r <= 1'b0;
    else if (!busy_r && ap_start_lite)      en_conv_r <= (op_code_w == OP_CONV);
    else if (busy_r && op_done)             en_conv_r <= 1'b0;
end
always @(posedge ap_clk) begin
    if (ap_rst_n_inv)                       en_dwconv_r <= 1'b0;
    else if (!busy_r && ap_start_lite)       en_dwconv_r <= (op_code_w == OP_DWCONV);
    else if (busy_r && op_done)              en_dwconv_r <= 1'b0;
end
always @(posedge ap_clk) begin
    if (ap_rst_n_inv)                       en_pwconv_r <= 1'b0;
    else if (!busy_r && ap_start_lite)       en_pwconv_r <= (op_code_w == OP_PWCONV);
    else if (busy_r && op_done)              en_pwconv_r <= 1'b0;
end
always @(posedge ap_clk) begin
    if (ap_rst_n_inv)                       en_add_r <= 1'b0;
    else if (!busy_r && ap_start_lite)       en_add_r <= (op_code_w == OP_ADD);
    else if (busy_r && op_done)              en_add_r <= 1'b0;
end
always @(posedge ap_clk) begin
    if (ap_rst_n_inv)                       en_gelu_r <= 1'b0;
    else if (!busy_r && ap_start_lite)       en_gelu_r <= (op_code_w == OP_GELU);
    else if (busy_r && op_done)              en_gelu_r <= 1'b0;
end

// One-shot ap_start pulse to the selected worker, held until it
// leaves idle (robust regardless of exact sub-core sampling timing).
reg add_start_hold;
always @(posedge ap_clk) begin
    if (ap_rst_n_inv) add_start_hold <= 1'b0;
    else if (!busy_r && ap_start_lite && (op_code_w == OP_ADD)) add_start_hold <= 1'b1;
    else if (add_start_hold && !add_worker_ap_idle)             add_start_hold <= 1'b0;
end
wire add_worker_ap_start = add_start_hold;

assign ap_idle_o  = !busy_r;
assign ap_done_o  = busy_r & op_done;
assign ap_ready_o = ap_done_o;

// ------------------------------------------------------------------
// add_worker (black box, unmodified) + its 2 private multiplier
// stubs (previously shared with the top FSM in the HLS-generated
// design -- here each add_worker instance gets its own copy, no
// cross-module sharing).
// ------------------------------------------------------------------
wire signed [31:0] add_mul4090_din0, add_mul4090_din1, add_mul4090_dout;
wire               add_mul4090_ce;
wire signed [31:0] add_mul4094_din0, add_mul4094_din1, add_mul4094_dout;
wire               add_mul4094_ce;

// add_worker's own private per-bundle AXI4 signals (feed into this
// worker's dedicated adapter instances below, not shared with anyone).
wire add_g0_AWVALID, add_g0_WVALID, add_g0_ARVALID, add_g0_RREADY, add_g0_BREADY;
wire [63:0] add_g0_AWADDR, add_g0_ARADDR;
wire [31:0]  add_g0_AWLEN, add_g0_ARLEN;
wire [31:0] add_g0_WDATA;
wire [3:0]  add_g0_WSTRB;
wire        add_g0_ARREADY_i, add_g0_RVALID_i;
wire [31:0] add_g0_RDATA_i;
wire [10:0] add_g0_RFIFONUM_i;
wire        add_g0_AWREADY_i, add_g0_WREADY_i, add_g0_BVALID_i;

wire add_g1_ARVALID, add_g1_RREADY;
wire [63:0] add_g1_ARADDR;
wire [31:0]  add_g1_ARLEN;
wire        add_g1_ARREADY_i, add_g1_RVALID_i;
wire [7:0]  add_g1_RDATA_i;
wire [12:0] add_g1_RFIFONUM_i;
wire        add_g1_AWREADY_i, add_g1_WREADY_i, add_g1_BVALID_i;

wire add_g3_AWVALID, add_g3_WVALID, add_g3_BREADY;
wire [63:0] add_g3_AWADDR;
wire [31:0]  add_g3_AWLEN;
wire [31:0] add_g3_WDATA;
wire [3:0]  add_g3_WSTRB;
wire        add_g3_AWREADY_i, add_g3_WREADY_i, add_g3_BVALID_i;
wire        add_g3_ARREADY_i, add_g3_RVALID_i;
wire [31:0] add_g3_RDATA_i;
wire [8:0]  add_g3_RFIFONUM_i;

fastvit_ip_add_worker add_worker_U (
    .ap_clk(ap_clk), .ap_rst(ap_rst_n_inv),
    .ap_start(add_worker_ap_start), .ap_done(add_worker_ap_done),
    .ap_idle(add_worker_ap_idle), .ap_ready(add_worker_ap_ready),

    // gmem0 (in_a, read-only)
    .m_axi_gmem0_0_AWVALID(add_g0_AWVALID), .m_axi_gmem0_0_AWREADY(1'b0),
    .m_axi_gmem0_0_AWADDR(add_g0_AWADDR), .m_axi_gmem0_0_AWID(), .m_axi_gmem0_0_AWLEN(add_g0_AWLEN),
    .m_axi_gmem0_0_AWSIZE(), .m_axi_gmem0_0_AWBURST(), .m_axi_gmem0_0_AWLOCK(),
    .m_axi_gmem0_0_AWCACHE(), .m_axi_gmem0_0_AWPROT(), .m_axi_gmem0_0_AWQOS(),
    .m_axi_gmem0_0_AWREGION(), .m_axi_gmem0_0_AWUSER(),
    .m_axi_gmem0_0_WVALID(add_g0_WVALID), .m_axi_gmem0_0_WREADY(1'b0),
    .m_axi_gmem0_0_WDATA(add_g0_WDATA), .m_axi_gmem0_0_WSTRB(add_g0_WSTRB),
    .m_axi_gmem0_0_WLAST(), .m_axi_gmem0_0_WID(), .m_axi_gmem0_0_WUSER(),
    .m_axi_gmem0_0_ARVALID(add_g0_ARVALID), .m_axi_gmem0_0_ARREADY(add_g0_ARREADY_i),
    .m_axi_gmem0_0_ARADDR(add_g0_ARADDR), .m_axi_gmem0_0_ARID(), .m_axi_gmem0_0_ARLEN(add_g0_ARLEN),
    .m_axi_gmem0_0_ARSIZE(), .m_axi_gmem0_0_ARBURST(), .m_axi_gmem0_0_ARLOCK(),
    .m_axi_gmem0_0_ARCACHE(), .m_axi_gmem0_0_ARPROT(), .m_axi_gmem0_0_ARQOS(),
    .m_axi_gmem0_0_ARREGION(), .m_axi_gmem0_0_ARUSER(),
    .m_axi_gmem0_0_RVALID(add_g0_RVALID_i), .m_axi_gmem0_0_RREADY(add_g0_RREADY),
    .m_axi_gmem0_0_RDATA(add_g0_RDATA_i), .m_axi_gmem0_0_RLAST(1'b0),
    .m_axi_gmem0_0_RID(1'd0), .m_axi_gmem0_0_RFIFONUM(add_g0_RFIFONUM_i),
    .m_axi_gmem0_0_RUSER(1'd0), .m_axi_gmem0_0_RRESP(2'd0),
    .m_axi_gmem0_0_BVALID(1'b0), .m_axi_gmem0_0_BREADY(add_g0_BREADY),
    .m_axi_gmem0_0_BRESP(2'd0), .m_axi_gmem0_0_BID(1'd0), .m_axi_gmem0_0_BUSER(1'd0),
    .in_a(in_a_addr),

    // gmem1 (in_b, read-only)
    .m_axi_gmem1_0_AWVALID(), .m_axi_gmem1_0_AWREADY(1'b0),
    .m_axi_gmem1_0_AWADDR(), .m_axi_gmem1_0_AWID(), .m_axi_gmem1_0_AWLEN(),
    .m_axi_gmem1_0_AWSIZE(), .m_axi_gmem1_0_AWBURST(), .m_axi_gmem1_0_AWLOCK(),
    .m_axi_gmem1_0_AWCACHE(), .m_axi_gmem1_0_AWPROT(), .m_axi_gmem1_0_AWQOS(),
    .m_axi_gmem1_0_AWREGION(), .m_axi_gmem1_0_AWUSER(),
    .m_axi_gmem1_0_WVALID(), .m_axi_gmem1_0_WREADY(1'b0),
    .m_axi_gmem1_0_WDATA(), .m_axi_gmem1_0_WSTRB(),
    .m_axi_gmem1_0_WLAST(), .m_axi_gmem1_0_WID(), .m_axi_gmem1_0_WUSER(),
    .m_axi_gmem1_0_ARVALID(add_g1_ARVALID), .m_axi_gmem1_0_ARREADY(add_g1_ARREADY_i),
    .m_axi_gmem1_0_ARADDR(add_g1_ARADDR), .m_axi_gmem1_0_ARID(), .m_axi_gmem1_0_ARLEN(add_g1_ARLEN),
    .m_axi_gmem1_0_ARSIZE(), .m_axi_gmem1_0_ARBURST(), .m_axi_gmem1_0_ARLOCK(),
    .m_axi_gmem1_0_ARCACHE(), .m_axi_gmem1_0_ARPROT(), .m_axi_gmem1_0_ARQOS(),
    .m_axi_gmem1_0_ARREGION(), .m_axi_gmem1_0_ARUSER(),
    .m_axi_gmem1_0_RVALID(add_g1_RVALID_i), .m_axi_gmem1_0_RREADY(add_g1_RREADY),
    .m_axi_gmem1_0_RDATA(add_g1_RDATA_i), .m_axi_gmem1_0_RLAST(1'b0),
    .m_axi_gmem1_0_RID(1'd0), .m_axi_gmem1_0_RFIFONUM(add_g1_RFIFONUM_i),
    .m_axi_gmem1_0_RUSER(1'd0), .m_axi_gmem1_0_RRESP(2'd0),
    .m_axi_gmem1_0_BVALID(1'b0), .m_axi_gmem1_0_BREADY(),
    .m_axi_gmem1_0_BRESP(2'd0), .m_axi_gmem1_0_BID(1'd0), .m_axi_gmem1_0_BUSER(1'd0),
    .in_b(in_b_addr),

    // gmem3 (out, write-only)
    .m_axi_gmem3_0_AWVALID(add_g3_AWVALID), .m_axi_gmem3_0_AWREADY(add_g3_AWREADY_i),
    .m_axi_gmem3_0_AWADDR(add_g3_AWADDR), .m_axi_gmem3_0_AWID(), .m_axi_gmem3_0_AWLEN(add_g3_AWLEN),
    .m_axi_gmem3_0_AWSIZE(), .m_axi_gmem3_0_AWBURST(), .m_axi_gmem3_0_AWLOCK(),
    .m_axi_gmem3_0_AWCACHE(), .m_axi_gmem3_0_AWPROT(), .m_axi_gmem3_0_AWQOS(),
    .m_axi_gmem3_0_AWREGION(), .m_axi_gmem3_0_AWUSER(),
    .m_axi_gmem3_0_WVALID(add_g3_WVALID), .m_axi_gmem3_0_WREADY(add_g3_WREADY_i),
    .m_axi_gmem3_0_WDATA(add_g3_WDATA), .m_axi_gmem3_0_WSTRB(add_g3_WSTRB),
    .m_axi_gmem3_0_WLAST(), .m_axi_gmem3_0_WID(), .m_axi_gmem3_0_WUSER(),
    .m_axi_gmem3_0_ARVALID(), .m_axi_gmem3_0_ARREADY(1'b0),
    .m_axi_gmem3_0_ARADDR(), .m_axi_gmem3_0_ARID(), .m_axi_gmem3_0_ARLEN(),
    .m_axi_gmem3_0_ARSIZE(), .m_axi_gmem3_0_ARBURST(), .m_axi_gmem3_0_ARLOCK(),
    .m_axi_gmem3_0_ARCACHE(), .m_axi_gmem3_0_ARPROT(), .m_axi_gmem3_0_ARQOS(),
    .m_axi_gmem3_0_ARREGION(), .m_axi_gmem3_0_ARUSER(),
    .m_axi_gmem3_0_RVALID(1'b0), .m_axi_gmem3_0_RREADY(),
    .m_axi_gmem3_0_RDATA(32'd0), .m_axi_gmem3_0_RLAST(1'b0),
    .m_axi_gmem3_0_RID(1'd0), .m_axi_gmem3_0_RFIFONUM(9'd0),
    .m_axi_gmem3_0_RUSER(1'd0), .m_axi_gmem3_0_RRESP(2'd0),
    .m_axi_gmem3_0_BVALID(add_g3_BVALID_i), .m_axi_gmem3_0_BREADY(add_g3_BREADY),
    .m_axi_gmem3_0_BRESP(2'd0), .m_axi_gmem3_0_BID(1'd0), .m_axi_gmem3_0_BUSER(1'd0),
    .out_r(out_r_addr),

    .CH(CHin_w), .H(Hin_w), .W(Win_w),

    .grp_fu_4090_p_din0(add_mul4090_din0), .grp_fu_4090_p_din1(add_mul4090_din1),
    .grp_fu_4090_p_dout0(add_mul4090_dout), .grp_fu_4090_p_ce(add_mul4090_ce),
    .grp_fu_4094_p_din0(add_mul4094_din0), .grp_fu_4094_p_din1(add_mul4094_din1),
    .grp_fu_4094_p_dout0(add_mul4094_dout), .grp_fu_4094_p_ce(add_mul4094_ce)
);

fastvit_ip_mul_32s_32s_32_5_1 #(.ID(1), .NUM_STAGE(5), .din0_WIDTH(32), .din1_WIDTH(32), .dout_WIDTH(32))
add_mul_4090_U (.clk(ap_clk), .reset(ap_rst_n_inv),
    .din0(add_mul4090_din0), .din1(add_mul4090_din1), .ce(add_mul4090_ce), .dout(add_mul4090_dout));

fastvit_ip_mul_32s_32s_32_5_1 #(.ID(1), .NUM_STAGE(5), .din0_WIDTH(32), .din1_WIDTH(32), .dout_WIDTH(32))
add_mul_4094_U (.clk(ap_clk), .reset(ap_rst_n_inv),
    .din0(add_mul4094_din0), .din1(add_mul4094_din1), .ce(add_mul4094_ce), .dout(add_mul4094_dout));

// ------------------------------------------------------------------
// Private per-worker gmemN burst/width adapters (unmodified black
// box, one instance per worker per bundle instead of one shared
// instance per bundle).
// ------------------------------------------------------------------






// ==================== dwconv_worker path ====================
/* dwconv_start_hold deassert condition (2026-08-07 bug fix): every
 * other worker in this file releases its ap_start hold as soon as
 * ap_idle first drops (one cycle), which is safe for a normal
 * FSM-based HLS block that latches ap_start into its own internal
 * register on the first cycle regardless of how long the caller holds
 * it. dwconv_worker stopped being that kind of block once this
 * session's LOAD_DW_IN rework caused HLS to emit dwconv_preload_channel/
 * dwconv_process_channel as a genuine DATAFLOW region instead of one
 * inlined FSM -- confirmed by reading fastvit_ip_dwconv_worker.v: its
 * internal `dataflow_in_loop_LOOP_DW_CH_1_U0`'s own ap_start port is a
 * DIRECT passthrough of dwconv_worker's top-level ap_start input, no
 * internal latch at all. The DATAFLOW region's consumer stage
 * (dwconv_process_channel4) gates its own start on
 * "ap_start && channel_has_data", so releasing ap_start after 1 cycle
 * (long before the producer stage has actually filled the channel,
 * confirmed via hierarchical $display probing: process_channel4's
 * start condition never fires because ap_start is already back to 0 by
 * the time ch_in_buf's stream channel becomes non-empty, ~870ns/~174
 * cycles later) permanently deadlocks it -- reproduced exactly as a
 * hang transitioning into OP_DWCONV in tb_fastvit_top_tierb.sv, ap_idle
 * drops and never returns. Fix: hold ap_start until ap_done instead of
 * until ap_idle first drops -- matches busy_r/en_dwconv_r's own clear
 * condition (also gated on op_done, which is dwconv_worker_ap_done for
 * this op), so the whole dispatch state clears together at the actual
 * end of the operation. */
reg dwconv_start_hold;
always @(posedge ap_clk) begin
    if (ap_rst_n_inv) dwconv_start_hold <= 1'b0;
    else if (!busy_r && ap_start_lite && (op_code_w == OP_DWCONV)) dwconv_start_hold <= 1'b1;
    else if (dwconv_start_hold && dwconv_worker_ap_done) dwconv_start_hold <= 1'b0;
end
wire dwconv_worker_ap_start = dwconv_start_hold;

wire dwconv_g0_AWVALID, dwconv_g0_WVALID, dwconv_g0_ARVALID, dwconv_g0_RREADY, dwconv_g0_BREADY;
wire [63:0] dwconv_g0_ARADDR;
wire [31:0]  dwconv_g0_ARLEN;
wire        dwconv_g0_ARREADY_i, dwconv_g0_RVALID_i;
wire [31:0]  dwconv_g0_RDATA_i;
wire [10:0] dwconv_g0_RFIFONUM_i;
wire        dwconv_g0_AWREADY_i, dwconv_g0_WREADY_i, dwconv_g0_BVALID_i;
wire dwconv_g1_AWVALID, dwconv_g1_WVALID, dwconv_g1_ARVALID, dwconv_g1_RREADY, dwconv_g1_BREADY;
wire [63:0] dwconv_g1_ARADDR;
wire [31:0]  dwconv_g1_ARLEN;
wire        dwconv_g1_ARREADY_i, dwconv_g1_RVALID_i;
wire [7:0]  dwconv_g1_RDATA_i;
wire [12:0] dwconv_g1_RFIFONUM_i;
wire        dwconv_g1_AWREADY_i, dwconv_g1_WREADY_i, dwconv_g1_BVALID_i;
wire dwconv_g2_AWVALID, dwconv_g2_WVALID, dwconv_g2_ARVALID, dwconv_g2_RREADY, dwconv_g2_BREADY;
wire [63:0] dwconv_g2_ARADDR;
wire [31:0]  dwconv_g2_ARLEN;
wire        dwconv_g2_ARREADY_i, dwconv_g2_RVALID_i;
wire [31:0]  dwconv_g2_RDATA_i;
wire [8:0] dwconv_g2_RFIFONUM_i;
wire        dwconv_g2_AWREADY_i, dwconv_g2_WREADY_i, dwconv_g2_BVALID_i;
wire dwconv_g3_AWVALID, dwconv_g3_WVALID, dwconv_g3_BREADY;
wire [63:0] dwconv_g3_AWADDR;
wire [31:0]  dwconv_g3_AWLEN;
wire [31:0] dwconv_g3_WDATA;
wire [3:0]  dwconv_g3_WSTRB;
wire        dwconv_g3_AWREADY_i, dwconv_g3_WREADY_i, dwconv_g3_BVALID_i;
wire        dwconv_g3_ARREADY_i, dwconv_g3_RVALID_i;
wire [31:0] dwconv_g3_RDATA_i;
wire [8:0]  dwconv_g3_RFIFONUM_i;

fastvit_ip_dwconv_worker dwconv_worker_U (
    .ap_clk(ap_clk), .ap_rst(ap_rst_n_inv),
    .ap_start(dwconv_worker_ap_start), .ap_done(dwconv_worker_ap_done),
    .ap_idle(dwconv_worker_ap_idle), .ap_ready(dwconv_worker_ap_ready),

    // gmem0 (feat_in, read)
    .m_axi_gmem0_0_AWVALID(dwconv_g0_AWVALID), .m_axi_gmem0_0_AWREADY(1'b0),
    .m_axi_gmem0_0_AWADDR(), .m_axi_gmem0_0_AWID(), .m_axi_gmem0_0_AWLEN(),
    .m_axi_gmem0_0_AWSIZE(), .m_axi_gmem0_0_AWBURST(), .m_axi_gmem0_0_AWLOCK(),
    .m_axi_gmem0_0_AWCACHE(), .m_axi_gmem0_0_AWPROT(), .m_axi_gmem0_0_AWQOS(),
    .m_axi_gmem0_0_AWREGION(), .m_axi_gmem0_0_AWUSER(),
    .m_axi_gmem0_0_WVALID(dwconv_g0_WVALID), .m_axi_gmem0_0_WREADY(1'b0),
    .m_axi_gmem0_0_WDATA(), .m_axi_gmem0_0_WSTRB(),
    .m_axi_gmem0_0_WLAST(), .m_axi_gmem0_0_WID(), .m_axi_gmem0_0_WUSER(),
    .m_axi_gmem0_0_ARVALID(dwconv_g0_ARVALID), .m_axi_gmem0_0_ARREADY(dwconv_g0_ARREADY_i),
    .m_axi_gmem0_0_ARADDR(dwconv_g0_ARADDR), .m_axi_gmem0_0_ARID(), .m_axi_gmem0_0_ARLEN(dwconv_g0_ARLEN),
    .m_axi_gmem0_0_ARSIZE(), .m_axi_gmem0_0_ARBURST(), .m_axi_gmem0_0_ARLOCK(),
    .m_axi_gmem0_0_ARCACHE(), .m_axi_gmem0_0_ARPROT(), .m_axi_gmem0_0_ARQOS(),
    .m_axi_gmem0_0_ARREGION(), .m_axi_gmem0_0_ARUSER(),
    .m_axi_gmem0_0_RVALID(dwconv_g0_RVALID_i), .m_axi_gmem0_0_RREADY(dwconv_g0_RREADY),
    .m_axi_gmem0_0_RDATA(dwconv_g0_RDATA_i), .m_axi_gmem0_0_RLAST(1'b0),
    .m_axi_gmem0_0_RID(1'd0), .m_axi_gmem0_0_RFIFONUM(dwconv_g0_RFIFONUM_i),
    .m_axi_gmem0_0_RUSER(1'd0), .m_axi_gmem0_0_RRESP(2'd0),
    .m_axi_gmem0_0_BVALID(1'b0), .m_axi_gmem0_0_BREADY(dwconv_g0_BREADY),
    .m_axi_gmem0_0_BRESP(2'd0), .m_axi_gmem0_0_BID(1'd0), .m_axi_gmem0_0_BUSER(1'd0),
    .feat_in(in_a_addr),

    // gmem1 (weight, read)
    .m_axi_gmem1_0_AWVALID(dwconv_g1_AWVALID), .m_axi_gmem1_0_AWREADY(1'b0),
    .m_axi_gmem1_0_AWADDR(), .m_axi_gmem1_0_AWID(), .m_axi_gmem1_0_AWLEN(),
    .m_axi_gmem1_0_AWSIZE(), .m_axi_gmem1_0_AWBURST(), .m_axi_gmem1_0_AWLOCK(),
    .m_axi_gmem1_0_AWCACHE(), .m_axi_gmem1_0_AWPROT(), .m_axi_gmem1_0_AWQOS(),
    .m_axi_gmem1_0_AWREGION(), .m_axi_gmem1_0_AWUSER(),
    .m_axi_gmem1_0_WVALID(dwconv_g1_WVALID), .m_axi_gmem1_0_WREADY(1'b0),
    .m_axi_gmem1_0_WDATA(), .m_axi_gmem1_0_WSTRB(),
    .m_axi_gmem1_0_WLAST(), .m_axi_gmem1_0_WID(), .m_axi_gmem1_0_WUSER(),
    .m_axi_gmem1_0_ARVALID(dwconv_g1_ARVALID), .m_axi_gmem1_0_ARREADY(dwconv_g1_ARREADY_i),
    .m_axi_gmem1_0_ARADDR(dwconv_g1_ARADDR), .m_axi_gmem1_0_ARID(), .m_axi_gmem1_0_ARLEN(dwconv_g1_ARLEN),
    .m_axi_gmem1_0_ARSIZE(), .m_axi_gmem1_0_ARBURST(), .m_axi_gmem1_0_ARLOCK(),
    .m_axi_gmem1_0_ARCACHE(), .m_axi_gmem1_0_ARPROT(), .m_axi_gmem1_0_ARQOS(),
    .m_axi_gmem1_0_ARREGION(), .m_axi_gmem1_0_ARUSER(),
    .m_axi_gmem1_0_RVALID(dwconv_g1_RVALID_i), .m_axi_gmem1_0_RREADY(dwconv_g1_RREADY),
    .m_axi_gmem1_0_RDATA(dwconv_g1_RDATA_i), .m_axi_gmem1_0_RLAST(1'b0),
    .m_axi_gmem1_0_RID(1'd0), .m_axi_gmem1_0_RFIFONUM(dwconv_g1_RFIFONUM_i),
    .m_axi_gmem1_0_RUSER(1'd0), .m_axi_gmem1_0_RRESP(2'd0),
    .m_axi_gmem1_0_BVALID(1'b0), .m_axi_gmem1_0_BREADY(dwconv_g1_BREADY),
    .m_axi_gmem1_0_BRESP(2'd0), .m_axi_gmem1_0_BID(1'd0), .m_axi_gmem1_0_BUSER(1'd0),
    .weight(in_b_addr),

    // gmem2 (bias, read)
    .m_axi_gmem2_0_AWVALID(dwconv_g2_AWVALID), .m_axi_gmem2_0_AWREADY(1'b0),
    .m_axi_gmem2_0_AWADDR(), .m_axi_gmem2_0_AWID(), .m_axi_gmem2_0_AWLEN(),
    .m_axi_gmem2_0_AWSIZE(), .m_axi_gmem2_0_AWBURST(), .m_axi_gmem2_0_AWLOCK(),
    .m_axi_gmem2_0_AWCACHE(), .m_axi_gmem2_0_AWPROT(), .m_axi_gmem2_0_AWQOS(),
    .m_axi_gmem2_0_AWREGION(), .m_axi_gmem2_0_AWUSER(),
    .m_axi_gmem2_0_WVALID(dwconv_g2_WVALID), .m_axi_gmem2_0_WREADY(1'b0),
    .m_axi_gmem2_0_WDATA(), .m_axi_gmem2_0_WSTRB(),
    .m_axi_gmem2_0_WLAST(), .m_axi_gmem2_0_WID(), .m_axi_gmem2_0_WUSER(),
    .m_axi_gmem2_0_ARVALID(dwconv_g2_ARVALID), .m_axi_gmem2_0_ARREADY(dwconv_g2_ARREADY_i),
    .m_axi_gmem2_0_ARADDR(dwconv_g2_ARADDR), .m_axi_gmem2_0_ARID(), .m_axi_gmem2_0_ARLEN(dwconv_g2_ARLEN),
    .m_axi_gmem2_0_ARSIZE(), .m_axi_gmem2_0_ARBURST(), .m_axi_gmem2_0_ARLOCK(),
    .m_axi_gmem2_0_ARCACHE(), .m_axi_gmem2_0_ARPROT(), .m_axi_gmem2_0_ARQOS(),
    .m_axi_gmem2_0_ARREGION(), .m_axi_gmem2_0_ARUSER(),
    .m_axi_gmem2_0_RVALID(dwconv_g2_RVALID_i), .m_axi_gmem2_0_RREADY(dwconv_g2_RREADY),
    .m_axi_gmem2_0_RDATA(dwconv_g2_RDATA_i), .m_axi_gmem2_0_RLAST(1'b0),
    .m_axi_gmem2_0_RID(1'd0), .m_axi_gmem2_0_RFIFONUM(dwconv_g2_RFIFONUM_i),
    .m_axi_gmem2_0_RUSER(1'd0), .m_axi_gmem2_0_RRESP(2'd0),
    .m_axi_gmem2_0_BVALID(1'b0), .m_axi_gmem2_0_BREADY(dwconv_g2_BREADY),
    .m_axi_gmem2_0_BRESP(2'd0), .m_axi_gmem2_0_BID(1'd0), .m_axi_gmem2_0_BUSER(1'd0),
    .bias(bias_addr),

    // gmem3 (feat_out, write)
    .m_axi_gmem3_0_AWVALID(dwconv_g3_AWVALID), .m_axi_gmem3_0_AWREADY(dwconv_g3_AWREADY_i),
    .m_axi_gmem3_0_AWADDR(dwconv_g3_AWADDR), .m_axi_gmem3_0_AWID(), .m_axi_gmem3_0_AWLEN(dwconv_g3_AWLEN),
    .m_axi_gmem3_0_AWSIZE(), .m_axi_gmem3_0_AWBURST(), .m_axi_gmem3_0_AWLOCK(),
    .m_axi_gmem3_0_AWCACHE(), .m_axi_gmem3_0_AWPROT(), .m_axi_gmem3_0_AWQOS(),
    .m_axi_gmem3_0_AWREGION(), .m_axi_gmem3_0_AWUSER(),
    .m_axi_gmem3_0_WVALID(dwconv_g3_WVALID), .m_axi_gmem3_0_WREADY(dwconv_g3_WREADY_i),
    .m_axi_gmem3_0_WDATA(dwconv_g3_WDATA), .m_axi_gmem3_0_WSTRB(dwconv_g3_WSTRB),
    .m_axi_gmem3_0_WLAST(), .m_axi_gmem3_0_WID(), .m_axi_gmem3_0_WUSER(),
    .m_axi_gmem3_0_ARVALID(), .m_axi_gmem3_0_ARREADY(1'b0),
    .m_axi_gmem3_0_ARADDR(), .m_axi_gmem3_0_ARID(), .m_axi_gmem3_0_ARLEN(),
    .m_axi_gmem3_0_ARSIZE(), .m_axi_gmem3_0_ARBURST(), .m_axi_gmem3_0_ARLOCK(),
    .m_axi_gmem3_0_ARCACHE(), .m_axi_gmem3_0_ARPROT(), .m_axi_gmem3_0_ARQOS(),
    .m_axi_gmem3_0_ARREGION(), .m_axi_gmem3_0_ARUSER(),
    .m_axi_gmem3_0_RVALID(1'b0), .m_axi_gmem3_0_RREADY(),
    .m_axi_gmem3_0_RDATA(32'd0), .m_axi_gmem3_0_RLAST(1'b0),
    .m_axi_gmem3_0_RID(1'd0), .m_axi_gmem3_0_RFIFONUM(9'd0),
    .m_axi_gmem3_0_RUSER(1'd0), .m_axi_gmem3_0_RRESP(2'd0),
    .m_axi_gmem3_0_BVALID(dwconv_g3_BVALID_i), .m_axi_gmem3_0_BREADY(dwconv_g3_BREADY),
    .m_axi_gmem3_0_BRESP(2'd0), .m_axi_gmem3_0_BID(1'd0), .m_axi_gmem3_0_BUSER(1'd0),
    .feat_out(out_r_addr),

    .CHin(CHin_w),
    .Hin(Hin_w),
    .Win(Win_w),
    .Kh(Kh_w),
    .Kw(Kw_w),
    .stride_h(stride_h_w),
    .stride_w(stride_w_w),
    .pad_h(pad_h_w),
    .pad_w(pad_w_w),
    .fpg(fpg_w),
    .act_mode(act_mode_w),
    .out_shift(out_shift_w)
);

fastvit_ip_gmem0_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(70), .MAX_READ_BURST_LENGTH(256), .MAX_WRITE_BURST_LENGTH(16),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(11), .CH0_USER_DW(32), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(4), .NUM_WRITE_OUTSTANDING(0))
dwconv_gmem0_m_axi_U (
    .AWVALID(m_axi_dwconv_gmem0_AWVALID), .AWREADY(m_axi_dwconv_gmem0_AWREADY), .AWADDR(m_axi_dwconv_gmem0_AWADDR),
    .AWID(m_axi_dwconv_gmem0_AWID), .AWLEN(m_axi_dwconv_gmem0_AWLEN), .AWSIZE(m_axi_dwconv_gmem0_AWSIZE),
    .AWBURST(m_axi_dwconv_gmem0_AWBURST), .AWLOCK(m_axi_dwconv_gmem0_AWLOCK), .AWCACHE(m_axi_dwconv_gmem0_AWCACHE),
    .AWPROT(m_axi_dwconv_gmem0_AWPROT), .AWQOS(m_axi_dwconv_gmem0_AWQOS), .AWREGION(m_axi_dwconv_gmem0_AWREGION), .AWUSER(m_axi_dwconv_gmem0_AWUSER),
    .WVALID(m_axi_dwconv_gmem0_WVALID), .WREADY(m_axi_dwconv_gmem0_WREADY), .WDATA(m_axi_dwconv_gmem0_WDATA),
    .WSTRB(m_axi_dwconv_gmem0_WSTRB), .WLAST(m_axi_dwconv_gmem0_WLAST), .WID(m_axi_dwconv_gmem0_WID), .WUSER(m_axi_dwconv_gmem0_WUSER),
    .ARVALID(m_axi_dwconv_gmem0_ARVALID), .ARREADY(m_axi_dwconv_gmem0_ARREADY), .ARADDR(m_axi_dwconv_gmem0_ARADDR),
    .ARID(m_axi_dwconv_gmem0_ARID), .ARLEN(m_axi_dwconv_gmem0_ARLEN), .ARSIZE(m_axi_dwconv_gmem0_ARSIZE),
    .ARBURST(m_axi_dwconv_gmem0_ARBURST), .ARLOCK(m_axi_dwconv_gmem0_ARLOCK), .ARCACHE(m_axi_dwconv_gmem0_ARCACHE),
    .ARPROT(m_axi_dwconv_gmem0_ARPROT), .ARQOS(m_axi_dwconv_gmem0_ARQOS), .ARREGION(m_axi_dwconv_gmem0_ARREGION), .ARUSER(m_axi_dwconv_gmem0_ARUSER),
    .RVALID(m_axi_dwconv_gmem0_RVALID), .RREADY(m_axi_dwconv_gmem0_RREADY), .RDATA(m_axi_dwconv_gmem0_RDATA),
    .RLAST(m_axi_dwconv_gmem0_RLAST), .RID(m_axi_dwconv_gmem0_RID), .RUSER(m_axi_dwconv_gmem0_RUSER), .RRESP(m_axi_dwconv_gmem0_RRESP),
    .BVALID(m_axi_dwconv_gmem0_BVALID), .BREADY(m_axi_dwconv_gmem0_BREADY), .BRESP(m_axi_dwconv_gmem0_BRESP),
    .BID(m_axi_dwconv_gmem0_BID), .BUSER(m_axi_dwconv_gmem0_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(dwconv_g0_ARVALID), .I_CH0_ARREADY(dwconv_g0_ARREADY_i), .I_CH0_ARADDR(dwconv_g0_ARADDR), .I_CH0_ARLEN(dwconv_g0_ARLEN),
    .I_CH0_RVALID(dwconv_g0_RVALID_i), .I_CH0_RREADY(dwconv_g0_RREADY), .I_CH0_RDATA(dwconv_g0_RDATA_i), .I_CH0_RFIFONUM(dwconv_g0_RFIFONUM_i),
    .I_CH0_AWVALID(1'b0), .I_CH0_AWREADY(dwconv_g0_AWREADY_i), .I_CH0_AWADDR(64'd0), .I_CH0_AWLEN(32'd0),
    .I_CH0_WVALID(1'b0), .I_CH0_WREADY(dwconv_g0_WREADY_i), .I_CH0_WDATA(32'd0), .I_CH0_WSTRB(4'd0),
    .I_CH0_BVALID(dwconv_g0_BVALID_i), .I_CH0_BREADY(1'b0)
);

fastvit_ip_gmem1_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(70), .MAX_READ_BURST_LENGTH(256), .MAX_WRITE_BURST_LENGTH(16),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(13), .CH0_USER_DW(8), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(4), .NUM_WRITE_OUTSTANDING(0))
dwconv_gmem1_m_axi_U (
    .AWVALID(m_axi_dwconv_gmem1_AWVALID), .AWREADY(m_axi_dwconv_gmem1_AWREADY), .AWADDR(m_axi_dwconv_gmem1_AWADDR),
    .AWID(m_axi_dwconv_gmem1_AWID), .AWLEN(m_axi_dwconv_gmem1_AWLEN), .AWSIZE(m_axi_dwconv_gmem1_AWSIZE),
    .AWBURST(m_axi_dwconv_gmem1_AWBURST), .AWLOCK(m_axi_dwconv_gmem1_AWLOCK), .AWCACHE(m_axi_dwconv_gmem1_AWCACHE),
    .AWPROT(m_axi_dwconv_gmem1_AWPROT), .AWQOS(m_axi_dwconv_gmem1_AWQOS), .AWREGION(m_axi_dwconv_gmem1_AWREGION), .AWUSER(m_axi_dwconv_gmem1_AWUSER),
    .WVALID(m_axi_dwconv_gmem1_WVALID), .WREADY(m_axi_dwconv_gmem1_WREADY), .WDATA(m_axi_dwconv_gmem1_WDATA),
    .WSTRB(m_axi_dwconv_gmem1_WSTRB), .WLAST(m_axi_dwconv_gmem1_WLAST), .WID(m_axi_dwconv_gmem1_WID), .WUSER(m_axi_dwconv_gmem1_WUSER),
    .ARVALID(m_axi_dwconv_gmem1_ARVALID), .ARREADY(m_axi_dwconv_gmem1_ARREADY), .ARADDR(m_axi_dwconv_gmem1_ARADDR),
    .ARID(m_axi_dwconv_gmem1_ARID), .ARLEN(m_axi_dwconv_gmem1_ARLEN), .ARSIZE(m_axi_dwconv_gmem1_ARSIZE),
    .ARBURST(m_axi_dwconv_gmem1_ARBURST), .ARLOCK(m_axi_dwconv_gmem1_ARLOCK), .ARCACHE(m_axi_dwconv_gmem1_ARCACHE),
    .ARPROT(m_axi_dwconv_gmem1_ARPROT), .ARQOS(m_axi_dwconv_gmem1_ARQOS), .ARREGION(m_axi_dwconv_gmem1_ARREGION), .ARUSER(m_axi_dwconv_gmem1_ARUSER),
    .RVALID(m_axi_dwconv_gmem1_RVALID), .RREADY(m_axi_dwconv_gmem1_RREADY), .RDATA(m_axi_dwconv_gmem1_RDATA),
    .RLAST(m_axi_dwconv_gmem1_RLAST), .RID(m_axi_dwconv_gmem1_RID), .RUSER(m_axi_dwconv_gmem1_RUSER), .RRESP(m_axi_dwconv_gmem1_RRESP),
    .BVALID(m_axi_dwconv_gmem1_BVALID), .BREADY(m_axi_dwconv_gmem1_BREADY), .BRESP(m_axi_dwconv_gmem1_BRESP),
    .BID(m_axi_dwconv_gmem1_BID), .BUSER(m_axi_dwconv_gmem1_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(dwconv_g1_ARVALID), .I_CH0_ARREADY(dwconv_g1_ARREADY_i), .I_CH0_ARADDR(dwconv_g1_ARADDR), .I_CH0_ARLEN(dwconv_g1_ARLEN),
    .I_CH0_RVALID(dwconv_g1_RVALID_i), .I_CH0_RREADY(dwconv_g1_RREADY), .I_CH0_RDATA(dwconv_g1_RDATA_i), .I_CH0_RFIFONUM(dwconv_g1_RFIFONUM_i),
    .I_CH0_AWVALID(1'b0), .I_CH0_AWREADY(dwconv_g1_AWREADY_i), .I_CH0_AWADDR(64'd0), .I_CH0_AWLEN(32'd0),
    .I_CH0_WVALID(1'b0), .I_CH0_WREADY(dwconv_g1_WREADY_i), .I_CH0_WDATA(8'd0), .I_CH0_WSTRB(1'd0),
    .I_CH0_BVALID(dwconv_g1_BVALID_i), .I_CH0_BREADY(1'b0)
);




// ==================== pwconv_worker path ====================
reg pwconv_start_hold;
always @(posedge ap_clk) begin
    if (ap_rst_n_inv) pwconv_start_hold <= 1'b0;
    else if (!busy_r && ap_start_lite && (op_code_w == OP_PWCONV)) pwconv_start_hold <= 1'b1;
    else if (pwconv_start_hold && !pwconv_worker_ap_idle) pwconv_start_hold <= 1'b0;
end
wire pwconv_worker_ap_start = pwconv_start_hold;

wire pwconv_g0_AWVALID, pwconv_g0_WVALID, pwconv_g0_ARVALID, pwconv_g0_RREADY, pwconv_g0_BREADY;
wire [63:0] pwconv_g0_ARADDR;
wire [31:0]  pwconv_g0_ARLEN;
wire        pwconv_g0_ARREADY_i, pwconv_g0_RVALID_i;
wire [31:0]  pwconv_g0_RDATA_i;
wire [10:0] pwconv_g0_RFIFONUM_i;
wire        pwconv_g0_AWREADY_i, pwconv_g0_WREADY_i, pwconv_g0_BVALID_i;
wire pwconv_g1_AWVALID, pwconv_g1_WVALID, pwconv_g1_ARVALID, pwconv_g1_RREADY, pwconv_g1_BREADY;
wire [63:0] pwconv_g1_ARADDR;
wire [31:0]  pwconv_g1_ARLEN;
wire        pwconv_g1_ARREADY_i, pwconv_g1_RVALID_i;
wire [7:0]  pwconv_g1_RDATA_i;
wire [12:0] pwconv_g1_RFIFONUM_i;
wire        pwconv_g1_AWREADY_i, pwconv_g1_WREADY_i, pwconv_g1_BVALID_i;
wire pwconv_g2_AWVALID, pwconv_g2_WVALID, pwconv_g2_ARVALID, pwconv_g2_RREADY, pwconv_g2_BREADY;
wire [63:0] pwconv_g2_ARADDR;
wire [31:0]  pwconv_g2_ARLEN;
wire        pwconv_g2_ARREADY_i, pwconv_g2_RVALID_i;
wire [31:0]  pwconv_g2_RDATA_i;
wire [8:0] pwconv_g2_RFIFONUM_i;
wire        pwconv_g2_AWREADY_i, pwconv_g2_WREADY_i, pwconv_g2_BVALID_i;
wire pwconv_g3_AWVALID, pwconv_g3_WVALID, pwconv_g3_BREADY;
wire [63:0] pwconv_g3_AWADDR;
wire [31:0]  pwconv_g3_AWLEN;
wire [31:0] pwconv_g3_WDATA;
wire [3:0]  pwconv_g3_WSTRB;
wire        pwconv_g3_AWREADY_i, pwconv_g3_WREADY_i, pwconv_g3_BVALID_i;
wire        pwconv_g3_ARREADY_i, pwconv_g3_RVALID_i;
wire [31:0] pwconv_g3_RDATA_i;
wire [8:0]  pwconv_g3_RFIFONUM_i;

fastvit_ip_pwconv_worker pwconv_worker_U (
    .ap_clk(ap_clk), .ap_rst(ap_rst_n_inv),
    .ap_start(pwconv_worker_ap_start), .ap_done(pwconv_worker_ap_done),
    .ap_idle(pwconv_worker_ap_idle), .ap_ready(pwconv_worker_ap_ready),

    // gmem0 (feat_in_w32, read)
    .m_axi_gmem0_0_AWVALID(pwconv_g0_AWVALID), .m_axi_gmem0_0_AWREADY(1'b0),
    .m_axi_gmem0_0_AWADDR(), .m_axi_gmem0_0_AWID(), .m_axi_gmem0_0_AWLEN(),
    .m_axi_gmem0_0_AWSIZE(), .m_axi_gmem0_0_AWBURST(), .m_axi_gmem0_0_AWLOCK(),
    .m_axi_gmem0_0_AWCACHE(), .m_axi_gmem0_0_AWPROT(), .m_axi_gmem0_0_AWQOS(),
    .m_axi_gmem0_0_AWREGION(), .m_axi_gmem0_0_AWUSER(),
    .m_axi_gmem0_0_WVALID(pwconv_g0_WVALID), .m_axi_gmem0_0_WREADY(1'b0),
    .m_axi_gmem0_0_WDATA(), .m_axi_gmem0_0_WSTRB(),
    .m_axi_gmem0_0_WLAST(), .m_axi_gmem0_0_WID(), .m_axi_gmem0_0_WUSER(),
    .m_axi_gmem0_0_ARVALID(pwconv_g0_ARVALID), .m_axi_gmem0_0_ARREADY(pwconv_g0_ARREADY_i),
    .m_axi_gmem0_0_ARADDR(pwconv_g0_ARADDR), .m_axi_gmem0_0_ARID(), .m_axi_gmem0_0_ARLEN(pwconv_g0_ARLEN),
    .m_axi_gmem0_0_ARSIZE(), .m_axi_gmem0_0_ARBURST(), .m_axi_gmem0_0_ARLOCK(),
    .m_axi_gmem0_0_ARCACHE(), .m_axi_gmem0_0_ARPROT(), .m_axi_gmem0_0_ARQOS(),
    .m_axi_gmem0_0_ARREGION(), .m_axi_gmem0_0_ARUSER(),
    .m_axi_gmem0_0_RVALID(pwconv_g0_RVALID_i), .m_axi_gmem0_0_RREADY(pwconv_g0_RREADY),
    .m_axi_gmem0_0_RDATA(pwconv_g0_RDATA_i), .m_axi_gmem0_0_RLAST(1'b0),
    .m_axi_gmem0_0_RID(1'd0), .m_axi_gmem0_0_RFIFONUM(pwconv_g0_RFIFONUM_i),
    .m_axi_gmem0_0_RUSER(1'd0), .m_axi_gmem0_0_RRESP(2'd0),
    .m_axi_gmem0_0_BVALID(1'b0), .m_axi_gmem0_0_BREADY(pwconv_g0_BREADY),
    .m_axi_gmem0_0_BRESP(2'd0), .m_axi_gmem0_0_BID(1'd0), .m_axi_gmem0_0_BUSER(1'd0),
    .feat_in_w32(in_a_addr),

    // gmem1 (weight, read)
    .m_axi_gmem1_0_AWVALID(pwconv_g1_AWVALID), .m_axi_gmem1_0_AWREADY(1'b0),
    .m_axi_gmem1_0_AWADDR(), .m_axi_gmem1_0_AWID(), .m_axi_gmem1_0_AWLEN(),
    .m_axi_gmem1_0_AWSIZE(), .m_axi_gmem1_0_AWBURST(), .m_axi_gmem1_0_AWLOCK(),
    .m_axi_gmem1_0_AWCACHE(), .m_axi_gmem1_0_AWPROT(), .m_axi_gmem1_0_AWQOS(),
    .m_axi_gmem1_0_AWREGION(), .m_axi_gmem1_0_AWUSER(),
    .m_axi_gmem1_0_WVALID(pwconv_g1_WVALID), .m_axi_gmem1_0_WREADY(1'b0),
    .m_axi_gmem1_0_WDATA(), .m_axi_gmem1_0_WSTRB(),
    .m_axi_gmem1_0_WLAST(), .m_axi_gmem1_0_WID(), .m_axi_gmem1_0_WUSER(),
    .m_axi_gmem1_0_ARVALID(pwconv_g1_ARVALID), .m_axi_gmem1_0_ARREADY(pwconv_g1_ARREADY_i),
    .m_axi_gmem1_0_ARADDR(pwconv_g1_ARADDR), .m_axi_gmem1_0_ARID(), .m_axi_gmem1_0_ARLEN(pwconv_g1_ARLEN),
    .m_axi_gmem1_0_ARSIZE(), .m_axi_gmem1_0_ARBURST(), .m_axi_gmem1_0_ARLOCK(),
    .m_axi_gmem1_0_ARCACHE(), .m_axi_gmem1_0_ARPROT(), .m_axi_gmem1_0_ARQOS(),
    .m_axi_gmem1_0_ARREGION(), .m_axi_gmem1_0_ARUSER(),
    .m_axi_gmem1_0_RVALID(pwconv_g1_RVALID_i), .m_axi_gmem1_0_RREADY(pwconv_g1_RREADY),
    .m_axi_gmem1_0_RDATA(pwconv_g1_RDATA_i), .m_axi_gmem1_0_RLAST(1'b0),
    .m_axi_gmem1_0_RID(1'd0), .m_axi_gmem1_0_RFIFONUM(pwconv_g1_RFIFONUM_i),
    .m_axi_gmem1_0_RUSER(1'd0), .m_axi_gmem1_0_RRESP(2'd0),
    .m_axi_gmem1_0_BVALID(1'b0), .m_axi_gmem1_0_BREADY(pwconv_g1_BREADY),
    .m_axi_gmem1_0_BRESP(2'd0), .m_axi_gmem1_0_BID(1'd0), .m_axi_gmem1_0_BUSER(1'd0),
    .weight(in_b_addr),

    // gmem2 (bias, read)
    .m_axi_gmem2_0_AWVALID(pwconv_g2_AWVALID), .m_axi_gmem2_0_AWREADY(1'b0),
    .m_axi_gmem2_0_AWADDR(), .m_axi_gmem2_0_AWID(), .m_axi_gmem2_0_AWLEN(),
    .m_axi_gmem2_0_AWSIZE(), .m_axi_gmem2_0_AWBURST(), .m_axi_gmem2_0_AWLOCK(),
    .m_axi_gmem2_0_AWCACHE(), .m_axi_gmem2_0_AWPROT(), .m_axi_gmem2_0_AWQOS(),
    .m_axi_gmem2_0_AWREGION(), .m_axi_gmem2_0_AWUSER(),
    .m_axi_gmem2_0_WVALID(pwconv_g2_WVALID), .m_axi_gmem2_0_WREADY(1'b0),
    .m_axi_gmem2_0_WDATA(), .m_axi_gmem2_0_WSTRB(),
    .m_axi_gmem2_0_WLAST(), .m_axi_gmem2_0_WID(), .m_axi_gmem2_0_WUSER(),
    .m_axi_gmem2_0_ARVALID(pwconv_g2_ARVALID), .m_axi_gmem2_0_ARREADY(pwconv_g2_ARREADY_i),
    .m_axi_gmem2_0_ARADDR(pwconv_g2_ARADDR), .m_axi_gmem2_0_ARID(), .m_axi_gmem2_0_ARLEN(pwconv_g2_ARLEN),
    .m_axi_gmem2_0_ARSIZE(), .m_axi_gmem2_0_ARBURST(), .m_axi_gmem2_0_ARLOCK(),
    .m_axi_gmem2_0_ARCACHE(), .m_axi_gmem2_0_ARPROT(), .m_axi_gmem2_0_ARQOS(),
    .m_axi_gmem2_0_ARREGION(), .m_axi_gmem2_0_ARUSER(),
    .m_axi_gmem2_0_RVALID(pwconv_g2_RVALID_i), .m_axi_gmem2_0_RREADY(pwconv_g2_RREADY),
    .m_axi_gmem2_0_RDATA(pwconv_g2_RDATA_i), .m_axi_gmem2_0_RLAST(1'b0),
    .m_axi_gmem2_0_RID(1'd0), .m_axi_gmem2_0_RFIFONUM(pwconv_g2_RFIFONUM_i),
    .m_axi_gmem2_0_RUSER(1'd0), .m_axi_gmem2_0_RRESP(2'd0),
    .m_axi_gmem2_0_BVALID(1'b0), .m_axi_gmem2_0_BREADY(pwconv_g2_BREADY),
    .m_axi_gmem2_0_BRESP(2'd0), .m_axi_gmem2_0_BID(1'd0), .m_axi_gmem2_0_BUSER(1'd0),
    .bias(bias_addr),

    // gmem3 (feat_out_w32, write)
    .m_axi_gmem3_0_AWVALID(pwconv_g3_AWVALID), .m_axi_gmem3_0_AWREADY(pwconv_g3_AWREADY_i),
    .m_axi_gmem3_0_AWADDR(pwconv_g3_AWADDR), .m_axi_gmem3_0_AWID(), .m_axi_gmem3_0_AWLEN(pwconv_g3_AWLEN),
    .m_axi_gmem3_0_AWSIZE(), .m_axi_gmem3_0_AWBURST(), .m_axi_gmem3_0_AWLOCK(),
    .m_axi_gmem3_0_AWCACHE(), .m_axi_gmem3_0_AWPROT(), .m_axi_gmem3_0_AWQOS(),
    .m_axi_gmem3_0_AWREGION(), .m_axi_gmem3_0_AWUSER(),
    .m_axi_gmem3_0_WVALID(pwconv_g3_WVALID), .m_axi_gmem3_0_WREADY(pwconv_g3_WREADY_i),
    .m_axi_gmem3_0_WDATA(pwconv_g3_WDATA), .m_axi_gmem3_0_WSTRB(pwconv_g3_WSTRB),
    .m_axi_gmem3_0_WLAST(), .m_axi_gmem3_0_WID(), .m_axi_gmem3_0_WUSER(),
    .m_axi_gmem3_0_ARVALID(), .m_axi_gmem3_0_ARREADY(1'b0),
    .m_axi_gmem3_0_ARADDR(), .m_axi_gmem3_0_ARID(), .m_axi_gmem3_0_ARLEN(),
    .m_axi_gmem3_0_ARSIZE(), .m_axi_gmem3_0_ARBURST(), .m_axi_gmem3_0_ARLOCK(),
    .m_axi_gmem3_0_ARCACHE(), .m_axi_gmem3_0_ARPROT(), .m_axi_gmem3_0_ARQOS(),
    .m_axi_gmem3_0_ARREGION(), .m_axi_gmem3_0_ARUSER(),
    .m_axi_gmem3_0_RVALID(1'b0), .m_axi_gmem3_0_RREADY(),
    .m_axi_gmem3_0_RDATA(32'd0), .m_axi_gmem3_0_RLAST(1'b0),
    .m_axi_gmem3_0_RID(1'd0), .m_axi_gmem3_0_RFIFONUM(9'd0),
    .m_axi_gmem3_0_RUSER(1'd0), .m_axi_gmem3_0_RRESP(2'd0),
    .m_axi_gmem3_0_BVALID(pwconv_g3_BVALID_i), .m_axi_gmem3_0_BREADY(pwconv_g3_BREADY),
    .m_axi_gmem3_0_BRESP(2'd0), .m_axi_gmem3_0_BID(1'd0), .m_axi_gmem3_0_BUSER(1'd0),
    .feat_out_w32(out_r_addr),

    .CHin(CHin_w),
    .H(Hin_w),
    .W(Win_w),
    .CHout(CHout_w),
    .act_mode(act_mode_w),
    .out_shift(out_shift_w)
);

fastvit_ip_gmem0_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(70), .MAX_READ_BURST_LENGTH(256), .MAX_WRITE_BURST_LENGTH(16),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(11), .CH0_USER_DW(32), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(4), .NUM_WRITE_OUTSTANDING(0))
pwconv_gmem0_m_axi_U (
    .AWVALID(m_axi_pwconv_gmem0_AWVALID), .AWREADY(m_axi_pwconv_gmem0_AWREADY), .AWADDR(m_axi_pwconv_gmem0_AWADDR),
    .AWID(m_axi_pwconv_gmem0_AWID), .AWLEN(m_axi_pwconv_gmem0_AWLEN), .AWSIZE(m_axi_pwconv_gmem0_AWSIZE),
    .AWBURST(m_axi_pwconv_gmem0_AWBURST), .AWLOCK(m_axi_pwconv_gmem0_AWLOCK), .AWCACHE(m_axi_pwconv_gmem0_AWCACHE),
    .AWPROT(m_axi_pwconv_gmem0_AWPROT), .AWQOS(m_axi_pwconv_gmem0_AWQOS), .AWREGION(m_axi_pwconv_gmem0_AWREGION), .AWUSER(m_axi_pwconv_gmem0_AWUSER),
    .WVALID(m_axi_pwconv_gmem0_WVALID), .WREADY(m_axi_pwconv_gmem0_WREADY), .WDATA(m_axi_pwconv_gmem0_WDATA),
    .WSTRB(m_axi_pwconv_gmem0_WSTRB), .WLAST(m_axi_pwconv_gmem0_WLAST), .WID(m_axi_pwconv_gmem0_WID), .WUSER(m_axi_pwconv_gmem0_WUSER),
    .ARVALID(m_axi_pwconv_gmem0_ARVALID), .ARREADY(m_axi_pwconv_gmem0_ARREADY), .ARADDR(m_axi_pwconv_gmem0_ARADDR),
    .ARID(m_axi_pwconv_gmem0_ARID), .ARLEN(m_axi_pwconv_gmem0_ARLEN), .ARSIZE(m_axi_pwconv_gmem0_ARSIZE),
    .ARBURST(m_axi_pwconv_gmem0_ARBURST), .ARLOCK(m_axi_pwconv_gmem0_ARLOCK), .ARCACHE(m_axi_pwconv_gmem0_ARCACHE),
    .ARPROT(m_axi_pwconv_gmem0_ARPROT), .ARQOS(m_axi_pwconv_gmem0_ARQOS), .ARREGION(m_axi_pwconv_gmem0_ARREGION), .ARUSER(m_axi_pwconv_gmem0_ARUSER),
    .RVALID(m_axi_pwconv_gmem0_RVALID), .RREADY(m_axi_pwconv_gmem0_RREADY), .RDATA(m_axi_pwconv_gmem0_RDATA),
    .RLAST(m_axi_pwconv_gmem0_RLAST), .RID(m_axi_pwconv_gmem0_RID), .RUSER(m_axi_pwconv_gmem0_RUSER), .RRESP(m_axi_pwconv_gmem0_RRESP),
    .BVALID(m_axi_pwconv_gmem0_BVALID), .BREADY(m_axi_pwconv_gmem0_BREADY), .BRESP(m_axi_pwconv_gmem0_BRESP),
    .BID(m_axi_pwconv_gmem0_BID), .BUSER(m_axi_pwconv_gmem0_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(pwconv_g0_ARVALID), .I_CH0_ARREADY(pwconv_g0_ARREADY_i), .I_CH0_ARADDR(pwconv_g0_ARADDR), .I_CH0_ARLEN(pwconv_g0_ARLEN),
    .I_CH0_RVALID(pwconv_g0_RVALID_i), .I_CH0_RREADY(pwconv_g0_RREADY), .I_CH0_RDATA(pwconv_g0_RDATA_i), .I_CH0_RFIFONUM(pwconv_g0_RFIFONUM_i),
    .I_CH0_AWVALID(1'b0), .I_CH0_AWREADY(pwconv_g0_AWREADY_i), .I_CH0_AWADDR(64'd0), .I_CH0_AWLEN(32'd0),
    .I_CH0_WVALID(1'b0), .I_CH0_WREADY(pwconv_g0_WREADY_i), .I_CH0_WDATA(32'd0), .I_CH0_WSTRB(4'd0),
    .I_CH0_BVALID(pwconv_g0_BVALID_i), .I_CH0_BREADY(1'b0)
);

fastvit_ip_gmem1_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(70), .MAX_READ_BURST_LENGTH(256), .MAX_WRITE_BURST_LENGTH(16),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(13), .CH0_USER_DW(8), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(4), .NUM_WRITE_OUTSTANDING(0))
pwconv_gmem1_m_axi_U (
    .AWVALID(m_axi_pwconv_gmem1_AWVALID), .AWREADY(m_axi_pwconv_gmem1_AWREADY), .AWADDR(m_axi_pwconv_gmem1_AWADDR),
    .AWID(m_axi_pwconv_gmem1_AWID), .AWLEN(m_axi_pwconv_gmem1_AWLEN), .AWSIZE(m_axi_pwconv_gmem1_AWSIZE),
    .AWBURST(m_axi_pwconv_gmem1_AWBURST), .AWLOCK(m_axi_pwconv_gmem1_AWLOCK), .AWCACHE(m_axi_pwconv_gmem1_AWCACHE),
    .AWPROT(m_axi_pwconv_gmem1_AWPROT), .AWQOS(m_axi_pwconv_gmem1_AWQOS), .AWREGION(m_axi_pwconv_gmem1_AWREGION), .AWUSER(m_axi_pwconv_gmem1_AWUSER),
    .WVALID(m_axi_pwconv_gmem1_WVALID), .WREADY(m_axi_pwconv_gmem1_WREADY), .WDATA(m_axi_pwconv_gmem1_WDATA),
    .WSTRB(m_axi_pwconv_gmem1_WSTRB), .WLAST(m_axi_pwconv_gmem1_WLAST), .WID(m_axi_pwconv_gmem1_WID), .WUSER(m_axi_pwconv_gmem1_WUSER),
    .ARVALID(m_axi_pwconv_gmem1_ARVALID), .ARREADY(m_axi_pwconv_gmem1_ARREADY), .ARADDR(m_axi_pwconv_gmem1_ARADDR),
    .ARID(m_axi_pwconv_gmem1_ARID), .ARLEN(m_axi_pwconv_gmem1_ARLEN), .ARSIZE(m_axi_pwconv_gmem1_ARSIZE),
    .ARBURST(m_axi_pwconv_gmem1_ARBURST), .ARLOCK(m_axi_pwconv_gmem1_ARLOCK), .ARCACHE(m_axi_pwconv_gmem1_ARCACHE),
    .ARPROT(m_axi_pwconv_gmem1_ARPROT), .ARQOS(m_axi_pwconv_gmem1_ARQOS), .ARREGION(m_axi_pwconv_gmem1_ARREGION), .ARUSER(m_axi_pwconv_gmem1_ARUSER),
    .RVALID(m_axi_pwconv_gmem1_RVALID), .RREADY(m_axi_pwconv_gmem1_RREADY), .RDATA(m_axi_pwconv_gmem1_RDATA),
    .RLAST(m_axi_pwconv_gmem1_RLAST), .RID(m_axi_pwconv_gmem1_RID), .RUSER(m_axi_pwconv_gmem1_RUSER), .RRESP(m_axi_pwconv_gmem1_RRESP),
    .BVALID(m_axi_pwconv_gmem1_BVALID), .BREADY(m_axi_pwconv_gmem1_BREADY), .BRESP(m_axi_pwconv_gmem1_BRESP),
    .BID(m_axi_pwconv_gmem1_BID), .BUSER(m_axi_pwconv_gmem1_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(pwconv_g1_ARVALID), .I_CH0_ARREADY(pwconv_g1_ARREADY_i), .I_CH0_ARADDR(pwconv_g1_ARADDR), .I_CH0_ARLEN(pwconv_g1_ARLEN),
    .I_CH0_RVALID(pwconv_g1_RVALID_i), .I_CH0_RREADY(pwconv_g1_RREADY), .I_CH0_RDATA(pwconv_g1_RDATA_i), .I_CH0_RFIFONUM(pwconv_g1_RFIFONUM_i),
    .I_CH0_AWVALID(1'b0), .I_CH0_AWREADY(pwconv_g1_AWREADY_i), .I_CH0_AWADDR(64'd0), .I_CH0_AWLEN(32'd0),
    .I_CH0_WVALID(1'b0), .I_CH0_WREADY(pwconv_g1_WREADY_i), .I_CH0_WDATA(8'd0), .I_CH0_WSTRB(1'd0),
    .I_CH0_BVALID(pwconv_g1_BVALID_i), .I_CH0_BREADY(1'b0)
);




// ==================== conv_worker path ====================
reg conv_start_hold;
always @(posedge ap_clk) begin
    if (ap_rst_n_inv) conv_start_hold <= 1'b0;
    else if (!busy_r && ap_start_lite && (op_code_w == OP_CONV)) conv_start_hold <= 1'b1;
    else if (conv_start_hold && !conv_worker_ap_idle) conv_start_hold <= 1'b0;
end
wire conv_worker_ap_start = conv_start_hold;

wire conv_g0_AWVALID, conv_g0_WVALID, conv_g0_ARVALID, conv_g0_RREADY, conv_g0_BREADY;
wire [63:0] conv_g0_ARADDR;
wire [31:0]  conv_g0_ARLEN;
wire        conv_g0_ARREADY_i, conv_g0_RVALID_i;
wire [31:0]  conv_g0_RDATA_i;
wire [10:0] conv_g0_RFIFONUM_i;
wire        conv_g0_AWREADY_i, conv_g0_WREADY_i, conv_g0_BVALID_i;
wire conv_g1_AWVALID, conv_g1_WVALID, conv_g1_ARVALID, conv_g1_RREADY, conv_g1_BREADY;
wire [63:0] conv_g1_ARADDR;
wire [31:0]  conv_g1_ARLEN;
wire        conv_g1_ARREADY_i, conv_g1_RVALID_i;
wire [7:0]  conv_g1_RDATA_i;
wire [12:0] conv_g1_RFIFONUM_i;
wire        conv_g1_AWREADY_i, conv_g1_WREADY_i, conv_g1_BVALID_i;
wire conv_g2_AWVALID, conv_g2_WVALID, conv_g2_ARVALID, conv_g2_RREADY, conv_g2_BREADY;
wire [63:0] conv_g2_ARADDR;
wire [31:0]  conv_g2_ARLEN;
wire        conv_g2_ARREADY_i, conv_g2_RVALID_i;
wire [31:0]  conv_g2_RDATA_i;
wire [8:0] conv_g2_RFIFONUM_i;
wire        conv_g2_AWREADY_i, conv_g2_WREADY_i, conv_g2_BVALID_i;
wire conv_g3_AWVALID, conv_g3_WVALID, conv_g3_BREADY;
wire [63:0] conv_g3_AWADDR;
wire [31:0]  conv_g3_AWLEN;
wire [31:0] conv_g3_WDATA;
wire [3:0]  conv_g3_WSTRB;
wire        conv_g3_AWREADY_i, conv_g3_WREADY_i, conv_g3_BVALID_i;
wire        conv_g3_ARREADY_i, conv_g3_RVALID_i;
wire [31:0] conv_g3_RDATA_i;
wire [8:0]  conv_g3_RFIFONUM_i;

fastvit_ip_conv_worker conv_worker_U (
    .ap_clk(ap_clk), .ap_rst(ap_rst_n_inv),
    .ap_start(conv_worker_ap_start), .ap_done(conv_worker_ap_done),
    .ap_idle(conv_worker_ap_idle), .ap_ready(conv_worker_ap_ready),

    // gmem0 (in_a, read)
    .m_axi_gmem0_0_AWVALID(conv_g0_AWVALID), .m_axi_gmem0_0_AWREADY(1'b0),
    .m_axi_gmem0_0_AWADDR(), .m_axi_gmem0_0_AWID(), .m_axi_gmem0_0_AWLEN(),
    .m_axi_gmem0_0_AWSIZE(), .m_axi_gmem0_0_AWBURST(), .m_axi_gmem0_0_AWLOCK(),
    .m_axi_gmem0_0_AWCACHE(), .m_axi_gmem0_0_AWPROT(), .m_axi_gmem0_0_AWQOS(),
    .m_axi_gmem0_0_AWREGION(), .m_axi_gmem0_0_AWUSER(),
    .m_axi_gmem0_0_WVALID(conv_g0_WVALID), .m_axi_gmem0_0_WREADY(1'b0),
    .m_axi_gmem0_0_WDATA(), .m_axi_gmem0_0_WSTRB(),
    .m_axi_gmem0_0_WLAST(), .m_axi_gmem0_0_WID(), .m_axi_gmem0_0_WUSER(),
    .m_axi_gmem0_0_ARVALID(conv_g0_ARVALID), .m_axi_gmem0_0_ARREADY(conv_g0_ARREADY_i),
    .m_axi_gmem0_0_ARADDR(conv_g0_ARADDR), .m_axi_gmem0_0_ARID(), .m_axi_gmem0_0_ARLEN(conv_g0_ARLEN),
    .m_axi_gmem0_0_ARSIZE(), .m_axi_gmem0_0_ARBURST(), .m_axi_gmem0_0_ARLOCK(),
    .m_axi_gmem0_0_ARCACHE(), .m_axi_gmem0_0_ARPROT(), .m_axi_gmem0_0_ARQOS(),
    .m_axi_gmem0_0_ARREGION(), .m_axi_gmem0_0_ARUSER(),
    .m_axi_gmem0_0_RVALID(conv_g0_RVALID_i), .m_axi_gmem0_0_RREADY(conv_g0_RREADY),
    .m_axi_gmem0_0_RDATA(conv_g0_RDATA_i), .m_axi_gmem0_0_RLAST(1'b0),
    .m_axi_gmem0_0_RID(1'd0), .m_axi_gmem0_0_RFIFONUM(conv_g0_RFIFONUM_i),
    .m_axi_gmem0_0_RUSER(1'd0), .m_axi_gmem0_0_RRESP(2'd0),
    .m_axi_gmem0_0_BVALID(1'b0), .m_axi_gmem0_0_BREADY(conv_g0_BREADY),
    .m_axi_gmem0_0_BRESP(2'd0), .m_axi_gmem0_0_BID(1'd0), .m_axi_gmem0_0_BUSER(1'd0),
    .in_a(in_a_addr),

    // gmem1 (in_b, read)
    .m_axi_gmem1_0_AWVALID(conv_g1_AWVALID), .m_axi_gmem1_0_AWREADY(1'b0),
    .m_axi_gmem1_0_AWADDR(), .m_axi_gmem1_0_AWID(), .m_axi_gmem1_0_AWLEN(),
    .m_axi_gmem1_0_AWSIZE(), .m_axi_gmem1_0_AWBURST(), .m_axi_gmem1_0_AWLOCK(),
    .m_axi_gmem1_0_AWCACHE(), .m_axi_gmem1_0_AWPROT(), .m_axi_gmem1_0_AWQOS(),
    .m_axi_gmem1_0_AWREGION(), .m_axi_gmem1_0_AWUSER(),
    .m_axi_gmem1_0_WVALID(conv_g1_WVALID), .m_axi_gmem1_0_WREADY(1'b0),
    .m_axi_gmem1_0_WDATA(), .m_axi_gmem1_0_WSTRB(),
    .m_axi_gmem1_0_WLAST(), .m_axi_gmem1_0_WID(), .m_axi_gmem1_0_WUSER(),
    .m_axi_gmem1_0_ARVALID(conv_g1_ARVALID), .m_axi_gmem1_0_ARREADY(conv_g1_ARREADY_i),
    .m_axi_gmem1_0_ARADDR(conv_g1_ARADDR), .m_axi_gmem1_0_ARID(), .m_axi_gmem1_0_ARLEN(conv_g1_ARLEN),
    .m_axi_gmem1_0_ARSIZE(), .m_axi_gmem1_0_ARBURST(), .m_axi_gmem1_0_ARLOCK(),
    .m_axi_gmem1_0_ARCACHE(), .m_axi_gmem1_0_ARPROT(), .m_axi_gmem1_0_ARQOS(),
    .m_axi_gmem1_0_ARREGION(), .m_axi_gmem1_0_ARUSER(),
    .m_axi_gmem1_0_RVALID(conv_g1_RVALID_i), .m_axi_gmem1_0_RREADY(conv_g1_RREADY),
    .m_axi_gmem1_0_RDATA(conv_g1_RDATA_i), .m_axi_gmem1_0_RLAST(1'b0),
    .m_axi_gmem1_0_RID(1'd0), .m_axi_gmem1_0_RFIFONUM(conv_g1_RFIFONUM_i),
    .m_axi_gmem1_0_RUSER(1'd0), .m_axi_gmem1_0_RRESP(2'd0),
    .m_axi_gmem1_0_BVALID(1'b0), .m_axi_gmem1_0_BREADY(conv_g1_BREADY),
    .m_axi_gmem1_0_BRESP(2'd0), .m_axi_gmem1_0_BID(1'd0), .m_axi_gmem1_0_BUSER(1'd0),
    .in_b(in_b_addr),

    // gmem2 (bias, read)
    .m_axi_gmem2_0_AWVALID(conv_g2_AWVALID), .m_axi_gmem2_0_AWREADY(1'b0),
    .m_axi_gmem2_0_AWADDR(), .m_axi_gmem2_0_AWID(), .m_axi_gmem2_0_AWLEN(),
    .m_axi_gmem2_0_AWSIZE(), .m_axi_gmem2_0_AWBURST(), .m_axi_gmem2_0_AWLOCK(),
    .m_axi_gmem2_0_AWCACHE(), .m_axi_gmem2_0_AWPROT(), .m_axi_gmem2_0_AWQOS(),
    .m_axi_gmem2_0_AWREGION(), .m_axi_gmem2_0_AWUSER(),
    .m_axi_gmem2_0_WVALID(conv_g2_WVALID), .m_axi_gmem2_0_WREADY(1'b0),
    .m_axi_gmem2_0_WDATA(), .m_axi_gmem2_0_WSTRB(),
    .m_axi_gmem2_0_WLAST(), .m_axi_gmem2_0_WID(), .m_axi_gmem2_0_WUSER(),
    .m_axi_gmem2_0_ARVALID(conv_g2_ARVALID), .m_axi_gmem2_0_ARREADY(conv_g2_ARREADY_i),
    .m_axi_gmem2_0_ARADDR(conv_g2_ARADDR), .m_axi_gmem2_0_ARID(), .m_axi_gmem2_0_ARLEN(conv_g2_ARLEN),
    .m_axi_gmem2_0_ARSIZE(), .m_axi_gmem2_0_ARBURST(), .m_axi_gmem2_0_ARLOCK(),
    .m_axi_gmem2_0_ARCACHE(), .m_axi_gmem2_0_ARPROT(), .m_axi_gmem2_0_ARQOS(),
    .m_axi_gmem2_0_ARREGION(), .m_axi_gmem2_0_ARUSER(),
    .m_axi_gmem2_0_RVALID(conv_g2_RVALID_i), .m_axi_gmem2_0_RREADY(conv_g2_RREADY),
    .m_axi_gmem2_0_RDATA(conv_g2_RDATA_i), .m_axi_gmem2_0_RLAST(1'b0),
    .m_axi_gmem2_0_RID(1'd0), .m_axi_gmem2_0_RFIFONUM(conv_g2_RFIFONUM_i),
    .m_axi_gmem2_0_RUSER(1'd0), .m_axi_gmem2_0_RRESP(2'd0),
    .m_axi_gmem2_0_BVALID(1'b0), .m_axi_gmem2_0_BREADY(conv_g2_BREADY),
    .m_axi_gmem2_0_BRESP(2'd0), .m_axi_gmem2_0_BID(1'd0), .m_axi_gmem2_0_BUSER(1'd0),
    .bias(bias_addr),

    // gmem3 (out_r, write)
    .m_axi_gmem3_0_AWVALID(conv_g3_AWVALID), .m_axi_gmem3_0_AWREADY(conv_g3_AWREADY_i),
    .m_axi_gmem3_0_AWADDR(conv_g3_AWADDR), .m_axi_gmem3_0_AWID(), .m_axi_gmem3_0_AWLEN(conv_g3_AWLEN),
    .m_axi_gmem3_0_AWSIZE(), .m_axi_gmem3_0_AWBURST(), .m_axi_gmem3_0_AWLOCK(),
    .m_axi_gmem3_0_AWCACHE(), .m_axi_gmem3_0_AWPROT(), .m_axi_gmem3_0_AWQOS(),
    .m_axi_gmem3_0_AWREGION(), .m_axi_gmem3_0_AWUSER(),
    .m_axi_gmem3_0_WVALID(conv_g3_WVALID), .m_axi_gmem3_0_WREADY(conv_g3_WREADY_i),
    .m_axi_gmem3_0_WDATA(conv_g3_WDATA), .m_axi_gmem3_0_WSTRB(conv_g3_WSTRB),
    .m_axi_gmem3_0_WLAST(), .m_axi_gmem3_0_WID(), .m_axi_gmem3_0_WUSER(),
    .m_axi_gmem3_0_ARVALID(), .m_axi_gmem3_0_ARREADY(1'b0),
    .m_axi_gmem3_0_ARADDR(), .m_axi_gmem3_0_ARID(), .m_axi_gmem3_0_ARLEN(),
    .m_axi_gmem3_0_ARSIZE(), .m_axi_gmem3_0_ARBURST(), .m_axi_gmem3_0_ARLOCK(),
    .m_axi_gmem3_0_ARCACHE(), .m_axi_gmem3_0_ARPROT(), .m_axi_gmem3_0_ARQOS(),
    .m_axi_gmem3_0_ARREGION(), .m_axi_gmem3_0_ARUSER(),
    .m_axi_gmem3_0_RVALID(1'b0), .m_axi_gmem3_0_RREADY(),
    .m_axi_gmem3_0_RDATA(32'd0), .m_axi_gmem3_0_RLAST(1'b0),
    .m_axi_gmem3_0_RID(1'd0), .m_axi_gmem3_0_RFIFONUM(9'd0),
    .m_axi_gmem3_0_RUSER(1'd0), .m_axi_gmem3_0_RRESP(2'd0),
    .m_axi_gmem3_0_BVALID(conv_g3_BVALID_i), .m_axi_gmem3_0_BREADY(conv_g3_BREADY),
    .m_axi_gmem3_0_BRESP(2'd0), .m_axi_gmem3_0_BID(1'd0), .m_axi_gmem3_0_BUSER(1'd0),
    .out_r(out_r_addr),

    .CHin(CHin_w),
    .Hin(Hin_w),
    .Win(Win_w),
    .CHout(CHout_w),
    .stride_h(stride_h_w),
    .stride_w(stride_w_w),
    .pad_h(pad_h_w),
    .pad_w(pad_w_w),
    .act_mode(act_mode_w),
    .out_shift(out_shift_w)
);









// ==================== gelu (hand-written FSM) ====================
// Single-beat request/response loop, one 32-bit word per request. NOTE:
// I_CH0_ARLEN/AWLEN on fastvit_ip_gmemN_m_axi is NOT the physical AXI4
// burst-length field -- it's a user-side "word count" consumed by the
// adapter's internal request FIFO/preprocessor (fastvit_ip_gmemN_m_axi's
// ARADDR PREPROCESSOR: valid_length = (rreq_len!=0) && !rreq_len[31]).
// A count of 0 is treated as invalid and the request silently never
// reaches the physical bus (I_CH0_ARREADY still accepts it into the
// FIFO, masking the problem -- confirmed by direct read of the adapter
// source after tracing a permanent RVALID hang here). Must be >=1; set
// to 1 for a single 32-bit word per request, matching the adapter's
// internal burst_converter which coalesces sequential single-word
// requests into real AXI4 bursts transparently. Simpler and lower-risk
// than trying to manage multi-beat ARLEN/AWLEN bookkeeping by hand for
// a first pass; gelu is a tiny fraction of total runtime so this is not
// a performance concern.

wire gelu_g0_ARVALID, gelu_g0_RREADY;
wire [63:0] gelu_g0_ARADDR;
wire [31:0]  gelu_g0_ARLEN;
wire        gelu_g0_ARREADY_i, gelu_g0_RVALID_i;
wire [31:0] gelu_g0_RDATA_i;
wire [10:0] gelu_g0_RFIFONUM_i;
wire        gelu_g0_AWREADY_i, gelu_g0_WREADY_i, gelu_g0_BVALID_i;

wire gelu_g3_AWVALID, gelu_g3_WVALID, gelu_g3_BREADY;
wire [63:0] gelu_g3_AWADDR;
wire [31:0]  gelu_g3_AWLEN;
wire [31:0] gelu_g3_WDATA;
wire [3:0]  gelu_g3_WSTRB;
wire        gelu_g3_AWREADY_i, gelu_g3_WREADY_i, gelu_g3_BVALID_i;
wire        gelu_g3_ARREADY_i, gelu_g3_RVALID_i;
wire [31:0] gelu_g3_RDATA_i;
wire [8:0]  gelu_g3_RFIFONUM_i;

function signed [3:0] gelu_lut;
    input signed [3:0] a;
    reg [3:0] idx;
    begin
        idx = a + 4'sd8;
        case (idx)
            4'd0, 4'd1, 4'd2, 4'd3, 4'd4, 4'd5, 4'd6: gelu_lut = -4'sd1;
            4'd7, 4'd8:                               gelu_lut = 4'sd0;
            4'd9, 4'd10:                              gelu_lut = 4'sd1;
            4'd11:                                    gelu_lut = 4'sd2;
            4'd12:                                    gelu_lut = 4'sd3;
            4'd13:                                    gelu_lut = 4'sd4;
            4'd14:                                    gelu_lut = 4'sd5;
            4'd15:                                    gelu_lut = 4'sd6;
            default:                                  gelu_lut = 4'sd0;
        endcase
    end
endfunction

localparam GELU_IDLE       = 3'd0;
localparam GELU_REQ_READ   = 3'd1;
localparam GELU_REQ_WRITE  = 3'd2;
localparam GELU_DONE       = 3'd3;

reg [2:0]  gelu_st;
reg [31:0] gelu_n_words, gelu_word_idx;
reg [63:0] gelu_in_addr, gelu_out_addr;
reg [31:0] gelu_word_out_r;

reg        gelu_ar_valid_r, gelu_rready_r;
reg [63:0] gelu_araddr_r;
reg        gelu_aw_valid_r, gelu_w_valid_r, gelu_bready_r;
reg [63:0] gelu_awaddr_r;

assign gelu_g0_ARVALID = gelu_ar_valid_r;
assign gelu_g0_ARADDR  = gelu_araddr_r;
assign gelu_g0_ARLEN   = 32'd1;
assign gelu_g0_RREADY  = gelu_rready_r;
assign gelu_g3_AWVALID = gelu_aw_valid_r;
assign gelu_g3_AWADDR  = gelu_awaddr_r;
assign gelu_g3_AWLEN   = 32'd1;
assign gelu_g3_WVALID  = gelu_w_valid_r;
assign gelu_g3_WDATA   = gelu_word_out_r;
assign gelu_g3_WSTRB   = 4'hF;
assign gelu_g3_BREADY  = gelu_bready_r;

assign gelu_ap_idle = (gelu_st == GELU_IDLE);
assign gelu_ap_done = (gelu_st == GELU_DONE);

/* Pipelined precompute of gelu_n_words (2026-08-07 timing fix): the
 * original computed CHin_w*Hin_w*Win_w (a 32-bit triple multiply)
 * purely combinationally and used it the SAME cycle to decide
 * GELU_IDLE's next state/address -- real P&R measured this as the
 * worst path in the whole combined Tier B design, 18.537ns end-to-end
 * (14 logic levels, 2x DSP48E1 cascade) from a mid-multiply register
 * straight into gelu_araddr_r's address logic. Since CHin_w/Hin_w/Win_w
 * are AXI-Lite parameter registers written (and stable) many cycles
 * before ap_start is ever pulsed (the real driver writes all 13 scalar
 * params, then ap_start, as separate AXI-Lite transactions), a
 * free-running pipeline computing this in the background will always
 * have settled to the correct value well before GELU_IDLE samples it --
 * costs 3 extra registers, zero throughput impact. Same address-
 * pre-registration pattern already validated 3x this session on
 * dwconv_worker's LOAD_DW_IN and pwconv_worker's LOAD_PW_IN/
 * WRITE_PW_OUT -- see typed-knitting-nygaard.md. */
reg [31:0] gelu_chxh_r;
reg [31:0] gelu_total_px_r;
reg [31:0] gelu_n_words_p;
always @(posedge ap_clk) begin
    if (ap_rst_n_inv) begin
        gelu_chxh_r     <= 32'd0;
        gelu_total_px_r <= 32'd0;
        gelu_n_words_p  <= 32'd0;
    end else begin
        gelu_chxh_r     <= CHin_w * Hin_w;
        gelu_total_px_r <= gelu_chxh_r * Win_w;
        gelu_n_words_p  <= (gelu_total_px_r + 32'd7) >> 3;
    end
end

integer gb;
always @(posedge ap_clk) begin
    if (ap_rst_n_inv) begin
        gelu_st         <= GELU_IDLE;
        gelu_ar_valid_r <= 1'b0;
        gelu_rready_r   <= 1'b0;
        gelu_aw_valid_r <= 1'b0;
        gelu_w_valid_r  <= 1'b0;
        gelu_bready_r   <= 1'b0;
    end else begin
        case (gelu_st)
        GELU_IDLE: begin
            if (!busy_r && ap_start_lite && (op_code_w == OP_GELU)) begin
                gelu_n_words  <= gelu_n_words_p;
                gelu_word_idx <= 32'd0;
                gelu_in_addr  <= in_a_addr;
                gelu_out_addr <= out_r_addr;
                if (gelu_n_words_p == 32'd0) begin
                    gelu_st <= GELU_DONE;
                end else begin
                    gelu_st         <= GELU_REQ_READ;
                    gelu_araddr_r   <= in_a_addr >> 2;
                    gelu_ar_valid_r <= 1'b1;
                    gelu_rready_r   <= 1'b1;
                end
            end
        end
        GELU_REQ_READ: begin
            if (gelu_ar_valid_r && gelu_g0_ARREADY_i) gelu_ar_valid_r <= 1'b0;
            if (gelu_g0_RVALID_i && gelu_rready_r) begin
                for (gb = 0; gb < 8; gb = gb + 1)
                    gelu_word_out_r[gb*4 +: 4] <= gelu_lut($signed(gelu_g0_RDATA_i[gb*4 +: 4]));
                gelu_rready_r   <= 1'b0;
                gelu_st         <= GELU_REQ_WRITE;
                gelu_awaddr_r   <= gelu_out_addr >> 2;
                gelu_aw_valid_r <= 1'b1;
                gelu_w_valid_r  <= 1'b1;
                gelu_bready_r   <= 1'b1;
            end
        end
        GELU_REQ_WRITE: begin
            if (gelu_aw_valid_r && gelu_g3_AWREADY_i) gelu_aw_valid_r <= 1'b0;
            if (gelu_w_valid_r  && gelu_g3_WREADY_i)  gelu_w_valid_r  <= 1'b0;
            if (gelu_g3_BVALID_i && gelu_bready_r) begin
                gelu_bready_r <= 1'b0;
                if (gelu_word_idx + 32'd1 == gelu_n_words) begin
                    gelu_st <= GELU_DONE;
                end else begin
                    gelu_word_idx   <= gelu_word_idx + 32'd1;
                    gelu_in_addr    <= gelu_in_addr  + 64'd4;
                    gelu_out_addr   <= gelu_out_addr + 64'd4;
                    gelu_araddr_r   <= (gelu_in_addr  + 64'd4) >> 2;
                    gelu_ar_valid_r <= 1'b1;
                    gelu_rready_r   <= 1'b1;
                    gelu_st         <= GELU_REQ_READ;
                end
            end
        end
        GELU_DONE: begin
            gelu_st <= GELU_IDLE;
        end
        default: gelu_st <= GELU_IDLE;
        endcase
    end
end


// ==================================================================
// Shared adapters (partial-independence architecture): one physical
// adapter per gmem ROLE (not per worker), muxed by the existing
// independently-registered en_* dispatch signals. Only one worker is
// ever active at a time (mutually exclusive dispatch, enforced by the
// en_* always blocks above), so each mux is a plain select -- no
// shared high-fanout decode net (that was the actual Tier A/B
// bottleneck; op_code dispatch itself is unchanged from the
// full-independence version, still one dont_touch register per op).
// ==================================================================

// ---- shared_gmem0 (read-only): conv, add, gelu ----
wire        shared_gmem0_ARVALID = en_conv_r ? conv_g0_ARVALID : (en_add_r ? add_g0_ARVALID : (gelu_g0_ARVALID));
wire [63:0] shared_gmem0_ARADDR  = en_conv_r ? conv_g0_ARADDR : (en_add_r ? add_g0_ARADDR : (gelu_g0_ARADDR));
wire [31:0] shared_gmem0_ARLEN   = en_conv_r ? conv_g0_ARLEN : (en_add_r ? add_g0_ARLEN : (gelu_g0_ARLEN));
wire        shared_gmem0_RREADY  = en_conv_r ? conv_g0_RREADY : (en_add_r ? add_g0_RREADY : (gelu_g0_RREADY));
wire        shared_gmem0_ARREADY, shared_gmem0_RVALID;
wire [31:0] shared_gmem0_RDATA;
wire [10:0] shared_gmem0_RFIFONUM;

assign conv_g0_ARREADY_i  = en_conv_r ? shared_gmem0_ARREADY : 1'b0;
assign conv_g0_RVALID_i   = en_conv_r ? shared_gmem0_RVALID  : 1'b0;
assign conv_g0_RDATA_i    = shared_gmem0_RDATA;
assign conv_g0_RFIFONUM_i = shared_gmem0_RFIFONUM;
assign add_g0_ARREADY_i  = en_add_r ? shared_gmem0_ARREADY : 1'b0;
assign add_g0_RVALID_i   = en_add_r ? shared_gmem0_RVALID  : 1'b0;
assign add_g0_RDATA_i    = shared_gmem0_RDATA;
assign add_g0_RFIFONUM_i = shared_gmem0_RFIFONUM;
assign gelu_g0_ARREADY_i  = en_gelu_r ? shared_gmem0_ARREADY : 1'b0;
assign gelu_g0_RVALID_i   = en_gelu_r ? shared_gmem0_RVALID  : 1'b0;
assign gelu_g0_RDATA_i    = shared_gmem0_RDATA;
assign gelu_g0_RFIFONUM_i = shared_gmem0_RFIFONUM;

fastvit_ip_gmem0_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(70), .MAX_READ_BURST_LENGTH(256), .MAX_WRITE_BURST_LENGTH(16),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(11), .CH0_USER_DW(32), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(4), .NUM_WRITE_OUTSTANDING(0))
shared_gmem0_m_axi_U (
    .AWVALID(m_axi_shared_gmem0_AWVALID), .AWREADY(m_axi_shared_gmem0_AWREADY), .AWADDR(m_axi_shared_gmem0_AWADDR),
    .AWID(m_axi_shared_gmem0_AWID), .AWLEN(m_axi_shared_gmem0_AWLEN), .AWSIZE(m_axi_shared_gmem0_AWSIZE),
    .AWBURST(m_axi_shared_gmem0_AWBURST), .AWLOCK(m_axi_shared_gmem0_AWLOCK), .AWCACHE(m_axi_shared_gmem0_AWCACHE),
    .AWPROT(m_axi_shared_gmem0_AWPROT), .AWQOS(m_axi_shared_gmem0_AWQOS), .AWREGION(m_axi_shared_gmem0_AWREGION), .AWUSER(m_axi_shared_gmem0_AWUSER),
    .WVALID(m_axi_shared_gmem0_WVALID), .WREADY(m_axi_shared_gmem0_WREADY), .WDATA(m_axi_shared_gmem0_WDATA),
    .WSTRB(m_axi_shared_gmem0_WSTRB), .WLAST(m_axi_shared_gmem0_WLAST), .WID(m_axi_shared_gmem0_WID), .WUSER(m_axi_shared_gmem0_WUSER),
    .ARVALID(m_axi_shared_gmem0_ARVALID), .ARREADY(m_axi_shared_gmem0_ARREADY), .ARADDR(m_axi_shared_gmem0_ARADDR),
    .ARID(m_axi_shared_gmem0_ARID), .ARLEN(m_axi_shared_gmem0_ARLEN), .ARSIZE(m_axi_shared_gmem0_ARSIZE),
    .ARBURST(m_axi_shared_gmem0_ARBURST), .ARLOCK(m_axi_shared_gmem0_ARLOCK), .ARCACHE(m_axi_shared_gmem0_ARCACHE),
    .ARPROT(m_axi_shared_gmem0_ARPROT), .ARQOS(m_axi_shared_gmem0_ARQOS), .ARREGION(m_axi_shared_gmem0_ARREGION), .ARUSER(m_axi_shared_gmem0_ARUSER),
    .RVALID(m_axi_shared_gmem0_RVALID), .RREADY(m_axi_shared_gmem0_RREADY), .RDATA(m_axi_shared_gmem0_RDATA),
    .RLAST(m_axi_shared_gmem0_RLAST), .RID(m_axi_shared_gmem0_RID), .RUSER(m_axi_shared_gmem0_RUSER), .RRESP(m_axi_shared_gmem0_RRESP),
    .BVALID(m_axi_shared_gmem0_BVALID), .BREADY(m_axi_shared_gmem0_BREADY), .BRESP(m_axi_shared_gmem0_BRESP),
    .BID(m_axi_shared_gmem0_BID), .BUSER(m_axi_shared_gmem0_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(shared_gmem0_ARVALID), .I_CH0_ARREADY(shared_gmem0_ARREADY), .I_CH0_ARADDR(shared_gmem0_ARADDR), .I_CH0_ARLEN(shared_gmem0_ARLEN),
    .I_CH0_RVALID(shared_gmem0_RVALID), .I_CH0_RREADY(shared_gmem0_RREADY), .I_CH0_RDATA(shared_gmem0_RDATA), .I_CH0_RFIFONUM(shared_gmem0_RFIFONUM),
    .I_CH0_AWVALID(1'b0), .I_CH0_AWREADY(), .I_CH0_AWADDR(64'd0), .I_CH0_AWLEN(32'd0),
    .I_CH0_WVALID(1'b0), .I_CH0_WREADY(), .I_CH0_WDATA(32'd0), .I_CH0_WSTRB(4'd0),
    .I_CH0_BVALID(), .I_CH0_BREADY(1'b0)
);

// ---- shared_gmem1 (read-only): conv, add ----
wire        shared_gmem1_ARVALID = en_conv_r ? conv_g1_ARVALID : (add_g1_ARVALID);
wire [63:0] shared_gmem1_ARADDR  = en_conv_r ? conv_g1_ARADDR : (add_g1_ARADDR);
wire [31:0] shared_gmem1_ARLEN   = en_conv_r ? conv_g1_ARLEN : (add_g1_ARLEN);
wire        shared_gmem1_RREADY  = en_conv_r ? conv_g1_RREADY : (add_g1_RREADY);
wire        shared_gmem1_ARREADY, shared_gmem1_RVALID;
wire [7:0] shared_gmem1_RDATA;
wire [12:0] shared_gmem1_RFIFONUM;

assign conv_g1_ARREADY_i  = en_conv_r ? shared_gmem1_ARREADY : 1'b0;
assign conv_g1_RVALID_i   = en_conv_r ? shared_gmem1_RVALID  : 1'b0;
assign conv_g1_RDATA_i    = shared_gmem1_RDATA;
assign conv_g1_RFIFONUM_i = shared_gmem1_RFIFONUM;
assign add_g1_ARREADY_i  = en_add_r ? shared_gmem1_ARREADY : 1'b0;
assign add_g1_RVALID_i   = en_add_r ? shared_gmem1_RVALID  : 1'b0;
assign add_g1_RDATA_i    = shared_gmem1_RDATA;
assign add_g1_RFIFONUM_i = shared_gmem1_RFIFONUM;

fastvit_ip_gmem1_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(70), .MAX_READ_BURST_LENGTH(256), .MAX_WRITE_BURST_LENGTH(16),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(13), .CH0_USER_DW(8), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(4), .NUM_WRITE_OUTSTANDING(0))
shared_gmem1_m_axi_U (
    .AWVALID(m_axi_shared_gmem1_AWVALID), .AWREADY(m_axi_shared_gmem1_AWREADY), .AWADDR(m_axi_shared_gmem1_AWADDR),
    .AWID(m_axi_shared_gmem1_AWID), .AWLEN(m_axi_shared_gmem1_AWLEN), .AWSIZE(m_axi_shared_gmem1_AWSIZE),
    .AWBURST(m_axi_shared_gmem1_AWBURST), .AWLOCK(m_axi_shared_gmem1_AWLOCK), .AWCACHE(m_axi_shared_gmem1_AWCACHE),
    .AWPROT(m_axi_shared_gmem1_AWPROT), .AWQOS(m_axi_shared_gmem1_AWQOS), .AWREGION(m_axi_shared_gmem1_AWREGION), .AWUSER(m_axi_shared_gmem1_AWUSER),
    .WVALID(m_axi_shared_gmem1_WVALID), .WREADY(m_axi_shared_gmem1_WREADY), .WDATA(m_axi_shared_gmem1_WDATA),
    .WSTRB(m_axi_shared_gmem1_WSTRB), .WLAST(m_axi_shared_gmem1_WLAST), .WID(m_axi_shared_gmem1_WID), .WUSER(m_axi_shared_gmem1_WUSER),
    .ARVALID(m_axi_shared_gmem1_ARVALID), .ARREADY(m_axi_shared_gmem1_ARREADY), .ARADDR(m_axi_shared_gmem1_ARADDR),
    .ARID(m_axi_shared_gmem1_ARID), .ARLEN(m_axi_shared_gmem1_ARLEN), .ARSIZE(m_axi_shared_gmem1_ARSIZE),
    .ARBURST(m_axi_shared_gmem1_ARBURST), .ARLOCK(m_axi_shared_gmem1_ARLOCK), .ARCACHE(m_axi_shared_gmem1_ARCACHE),
    .ARPROT(m_axi_shared_gmem1_ARPROT), .ARQOS(m_axi_shared_gmem1_ARQOS), .ARREGION(m_axi_shared_gmem1_ARREGION), .ARUSER(m_axi_shared_gmem1_ARUSER),
    .RVALID(m_axi_shared_gmem1_RVALID), .RREADY(m_axi_shared_gmem1_RREADY), .RDATA(m_axi_shared_gmem1_RDATA),
    .RLAST(m_axi_shared_gmem1_RLAST), .RID(m_axi_shared_gmem1_RID), .RUSER(m_axi_shared_gmem1_RUSER), .RRESP(m_axi_shared_gmem1_RRESP),
    .BVALID(m_axi_shared_gmem1_BVALID), .BREADY(m_axi_shared_gmem1_BREADY), .BRESP(m_axi_shared_gmem1_BRESP),
    .BID(m_axi_shared_gmem1_BID), .BUSER(m_axi_shared_gmem1_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(shared_gmem1_ARVALID), .I_CH0_ARREADY(shared_gmem1_ARREADY), .I_CH0_ARADDR(shared_gmem1_ARADDR), .I_CH0_ARLEN(shared_gmem1_ARLEN),
    .I_CH0_RVALID(shared_gmem1_RVALID), .I_CH0_RREADY(shared_gmem1_RREADY), .I_CH0_RDATA(shared_gmem1_RDATA), .I_CH0_RFIFONUM(shared_gmem1_RFIFONUM),
    .I_CH0_AWVALID(1'b0), .I_CH0_AWREADY(), .I_CH0_AWADDR(64'd0), .I_CH0_AWLEN(32'd0),
    .I_CH0_WVALID(1'b0), .I_CH0_WREADY(), .I_CH0_WDATA(32'd0), .I_CH0_WSTRB(4'd0),
    .I_CH0_BVALID(), .I_CH0_BREADY(1'b0)
);

// ---- shared_gmem2 (read-only): conv, dwconv, pwconv ----
wire        shared_gmem2_ARVALID = en_conv_r ? conv_g2_ARVALID : (en_dwconv_r ? dwconv_g2_ARVALID : (pwconv_g2_ARVALID));
wire [63:0] shared_gmem2_ARADDR  = en_conv_r ? conv_g2_ARADDR : (en_dwconv_r ? dwconv_g2_ARADDR : (pwconv_g2_ARADDR));
wire [31:0] shared_gmem2_ARLEN   = en_conv_r ? conv_g2_ARLEN : (en_dwconv_r ? dwconv_g2_ARLEN : (pwconv_g2_ARLEN));
wire        shared_gmem2_RREADY  = en_conv_r ? conv_g2_RREADY : (en_dwconv_r ? dwconv_g2_RREADY : (pwconv_g2_RREADY));
wire        shared_gmem2_ARREADY, shared_gmem2_RVALID;
wire [31:0] shared_gmem2_RDATA;
wire [8:0] shared_gmem2_RFIFONUM;

assign conv_g2_ARREADY_i  = en_conv_r ? shared_gmem2_ARREADY : 1'b0;
assign conv_g2_RVALID_i   = en_conv_r ? shared_gmem2_RVALID  : 1'b0;
assign conv_g2_RDATA_i    = shared_gmem2_RDATA;
assign conv_g2_RFIFONUM_i = shared_gmem2_RFIFONUM;
assign dwconv_g2_ARREADY_i  = en_dwconv_r ? shared_gmem2_ARREADY : 1'b0;
assign dwconv_g2_RVALID_i   = en_dwconv_r ? shared_gmem2_RVALID  : 1'b0;
assign dwconv_g2_RDATA_i    = shared_gmem2_RDATA;
assign dwconv_g2_RFIFONUM_i = shared_gmem2_RFIFONUM;
assign pwconv_g2_ARREADY_i  = en_pwconv_r ? shared_gmem2_ARREADY : 1'b0;
assign pwconv_g2_RVALID_i   = en_pwconv_r ? shared_gmem2_RVALID  : 1'b0;
assign pwconv_g2_RDATA_i    = shared_gmem2_RDATA;
assign pwconv_g2_RFIFONUM_i = shared_gmem2_RFIFONUM;

fastvit_ip_gmem2_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(70), .MAX_READ_BURST_LENGTH(256), .MAX_WRITE_BURST_LENGTH(16),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(9), .CH0_USER_DW(32), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(16), .NUM_WRITE_OUTSTANDING(0))
shared_gmem2_m_axi_U (
    .AWVALID(m_axi_shared_gmem2_AWVALID), .AWREADY(m_axi_shared_gmem2_AWREADY), .AWADDR(m_axi_shared_gmem2_AWADDR),
    .AWID(m_axi_shared_gmem2_AWID), .AWLEN(m_axi_shared_gmem2_AWLEN), .AWSIZE(m_axi_shared_gmem2_AWSIZE),
    .AWBURST(m_axi_shared_gmem2_AWBURST), .AWLOCK(m_axi_shared_gmem2_AWLOCK), .AWCACHE(m_axi_shared_gmem2_AWCACHE),
    .AWPROT(m_axi_shared_gmem2_AWPROT), .AWQOS(m_axi_shared_gmem2_AWQOS), .AWREGION(m_axi_shared_gmem2_AWREGION), .AWUSER(m_axi_shared_gmem2_AWUSER),
    .WVALID(m_axi_shared_gmem2_WVALID), .WREADY(m_axi_shared_gmem2_WREADY), .WDATA(m_axi_shared_gmem2_WDATA),
    .WSTRB(m_axi_shared_gmem2_WSTRB), .WLAST(m_axi_shared_gmem2_WLAST), .WID(m_axi_shared_gmem2_WID), .WUSER(m_axi_shared_gmem2_WUSER),
    .ARVALID(m_axi_shared_gmem2_ARVALID), .ARREADY(m_axi_shared_gmem2_ARREADY), .ARADDR(m_axi_shared_gmem2_ARADDR),
    .ARID(m_axi_shared_gmem2_ARID), .ARLEN(m_axi_shared_gmem2_ARLEN), .ARSIZE(m_axi_shared_gmem2_ARSIZE),
    .ARBURST(m_axi_shared_gmem2_ARBURST), .ARLOCK(m_axi_shared_gmem2_ARLOCK), .ARCACHE(m_axi_shared_gmem2_ARCACHE),
    .ARPROT(m_axi_shared_gmem2_ARPROT), .ARQOS(m_axi_shared_gmem2_ARQOS), .ARREGION(m_axi_shared_gmem2_ARREGION), .ARUSER(m_axi_shared_gmem2_ARUSER),
    .RVALID(m_axi_shared_gmem2_RVALID), .RREADY(m_axi_shared_gmem2_RREADY), .RDATA(m_axi_shared_gmem2_RDATA),
    .RLAST(m_axi_shared_gmem2_RLAST), .RID(m_axi_shared_gmem2_RID), .RUSER(m_axi_shared_gmem2_RUSER), .RRESP(m_axi_shared_gmem2_RRESP),
    .BVALID(m_axi_shared_gmem2_BVALID), .BREADY(m_axi_shared_gmem2_BREADY), .BRESP(m_axi_shared_gmem2_BRESP),
    .BID(m_axi_shared_gmem2_BID), .BUSER(m_axi_shared_gmem2_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(shared_gmem2_ARVALID), .I_CH0_ARREADY(shared_gmem2_ARREADY), .I_CH0_ARADDR(shared_gmem2_ARADDR), .I_CH0_ARLEN(shared_gmem2_ARLEN),
    .I_CH0_RVALID(shared_gmem2_RVALID), .I_CH0_RREADY(shared_gmem2_RREADY), .I_CH0_RDATA(shared_gmem2_RDATA), .I_CH0_RFIFONUM(shared_gmem2_RFIFONUM),
    .I_CH0_AWVALID(1'b0), .I_CH0_AWREADY(), .I_CH0_AWADDR(64'd0), .I_CH0_AWLEN(32'd0),
    .I_CH0_WVALID(1'b0), .I_CH0_WREADY(), .I_CH0_WDATA(32'd0), .I_CH0_WSTRB(4'd0),
    .I_CH0_BVALID(), .I_CH0_BREADY(1'b0)
);

// ---- shared_gmem3 (write-only): conv, add, gelu, dwconv, pwconv ----
wire        shared_gmem3_AWVALID = en_conv_r ? conv_g3_AWVALID : (en_add_r ? add_g3_AWVALID : (en_gelu_r ? gelu_g3_AWVALID : (en_dwconv_r ? dwconv_g3_AWVALID : (pwconv_g3_AWVALID))));
wire [63:0] shared_gmem3_AWADDR  = en_conv_r ? conv_g3_AWADDR : (en_add_r ? add_g3_AWADDR : (en_gelu_r ? gelu_g3_AWADDR : (en_dwconv_r ? dwconv_g3_AWADDR : (pwconv_g3_AWADDR))));
wire [31:0] shared_gmem3_AWLEN   = en_conv_r ? conv_g3_AWLEN : (en_add_r ? add_g3_AWLEN : (en_gelu_r ? gelu_g3_AWLEN : (en_dwconv_r ? dwconv_g3_AWLEN : (pwconv_g3_AWLEN))));
wire        shared_gmem3_WVALID  = en_conv_r ? conv_g3_WVALID : (en_add_r ? add_g3_WVALID : (en_gelu_r ? gelu_g3_WVALID : (en_dwconv_r ? dwconv_g3_WVALID : (pwconv_g3_WVALID))));
wire [31:0] shared_gmem3_WDATA   = en_conv_r ? conv_g3_WDATA : (en_add_r ? add_g3_WDATA : (en_gelu_r ? gelu_g3_WDATA : (en_dwconv_r ? dwconv_g3_WDATA : (pwconv_g3_WDATA))));
wire [3:0]  shared_gmem3_WSTRB   = en_conv_r ? conv_g3_WSTRB : (en_add_r ? add_g3_WSTRB : (en_gelu_r ? gelu_g3_WSTRB : (en_dwconv_r ? dwconv_g3_WSTRB : (pwconv_g3_WSTRB))));
wire        shared_gmem3_BREADY  = en_conv_r ? conv_g3_BREADY : (en_add_r ? add_g3_BREADY : (en_gelu_r ? gelu_g3_BREADY : (en_dwconv_r ? dwconv_g3_BREADY : (pwconv_g3_BREADY))));
wire        shared_gmem3_AWREADY, shared_gmem3_WREADY, shared_gmem3_BVALID;

assign conv_g3_AWREADY_i = en_conv_r ? shared_gmem3_AWREADY : 1'b0;
assign conv_g3_WREADY_i  = en_conv_r ? shared_gmem3_WREADY  : 1'b0;
assign conv_g3_BVALID_i  = en_conv_r ? shared_gmem3_BVALID  : 1'b0;
assign add_g3_AWREADY_i = en_add_r ? shared_gmem3_AWREADY : 1'b0;
assign add_g3_WREADY_i  = en_add_r ? shared_gmem3_WREADY  : 1'b0;
assign add_g3_BVALID_i  = en_add_r ? shared_gmem3_BVALID  : 1'b0;
assign gelu_g3_AWREADY_i = en_gelu_r ? shared_gmem3_AWREADY : 1'b0;
assign gelu_g3_WREADY_i  = en_gelu_r ? shared_gmem3_WREADY  : 1'b0;
assign gelu_g3_BVALID_i  = en_gelu_r ? shared_gmem3_BVALID  : 1'b0;
assign dwconv_g3_AWREADY_i = en_dwconv_r ? shared_gmem3_AWREADY : 1'b0;
assign dwconv_g3_WREADY_i  = en_dwconv_r ? shared_gmem3_WREADY  : 1'b0;
assign dwconv_g3_BVALID_i  = en_dwconv_r ? shared_gmem3_BVALID  : 1'b0;
assign pwconv_g3_AWREADY_i = en_pwconv_r ? shared_gmem3_AWREADY : 1'b0;
assign pwconv_g3_WREADY_i  = en_pwconv_r ? shared_gmem3_WREADY  : 1'b0;
assign pwconv_g3_BVALID_i  = en_pwconv_r ? shared_gmem3_BVALID  : 1'b0;

fastvit_ip_gmem3_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(67), .MAX_READ_BURST_LENGTH(16), .MAX_WRITE_BURST_LENGTH(256),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(9), .CH0_USER_DW(32), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(0), .NUM_WRITE_OUTSTANDING(4))
shared_gmem3_m_axi_U (
    .AWVALID(m_axi_shared_gmem3_AWVALID), .AWREADY(m_axi_shared_gmem3_AWREADY), .AWADDR(m_axi_shared_gmem3_AWADDR),
    .AWID(m_axi_shared_gmem3_AWID), .AWLEN(m_axi_shared_gmem3_AWLEN), .AWSIZE(m_axi_shared_gmem3_AWSIZE),
    .AWBURST(m_axi_shared_gmem3_AWBURST), .AWLOCK(m_axi_shared_gmem3_AWLOCK), .AWCACHE(m_axi_shared_gmem3_AWCACHE),
    .AWPROT(m_axi_shared_gmem3_AWPROT), .AWQOS(m_axi_shared_gmem3_AWQOS), .AWREGION(m_axi_shared_gmem3_AWREGION), .AWUSER(m_axi_shared_gmem3_AWUSER),
    .WVALID(m_axi_shared_gmem3_WVALID), .WREADY(m_axi_shared_gmem3_WREADY), .WDATA(m_axi_shared_gmem3_WDATA),
    .WSTRB(m_axi_shared_gmem3_WSTRB), .WLAST(m_axi_shared_gmem3_WLAST), .WID(m_axi_shared_gmem3_WID), .WUSER(m_axi_shared_gmem3_WUSER),
    .ARVALID(m_axi_shared_gmem3_ARVALID), .ARREADY(m_axi_shared_gmem3_ARREADY), .ARADDR(m_axi_shared_gmem3_ARADDR),
    .ARID(m_axi_shared_gmem3_ARID), .ARLEN(m_axi_shared_gmem3_ARLEN), .ARSIZE(m_axi_shared_gmem3_ARSIZE),
    .ARBURST(m_axi_shared_gmem3_ARBURST), .ARLOCK(m_axi_shared_gmem3_ARLOCK), .ARCACHE(m_axi_shared_gmem3_ARCACHE),
    .ARPROT(m_axi_shared_gmem3_ARPROT), .ARQOS(m_axi_shared_gmem3_ARQOS), .ARREGION(m_axi_shared_gmem3_ARREGION), .ARUSER(m_axi_shared_gmem3_ARUSER),
    .RVALID(m_axi_shared_gmem3_RVALID), .RREADY(m_axi_shared_gmem3_RREADY), .RDATA(m_axi_shared_gmem3_RDATA),
    .RLAST(m_axi_shared_gmem3_RLAST), .RID(m_axi_shared_gmem3_RID), .RUSER(m_axi_shared_gmem3_RUSER), .RRESP(m_axi_shared_gmem3_RRESP),
    .BVALID(m_axi_shared_gmem3_BVALID), .BREADY(m_axi_shared_gmem3_BREADY), .BRESP(m_axi_shared_gmem3_BRESP),
    .BID(m_axi_shared_gmem3_BID), .BUSER(m_axi_shared_gmem3_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(1'b0), .I_CH0_ARREADY(), .I_CH0_ARADDR(64'd0), .I_CH0_ARLEN(32'd0),
    .I_CH0_RVALID(), .I_CH0_RREADY(1'b0), .I_CH0_RDATA(), .I_CH0_RFIFONUM(),
    .I_CH0_AWVALID(shared_gmem3_AWVALID), .I_CH0_AWREADY(shared_gmem3_AWREADY), .I_CH0_AWADDR(shared_gmem3_AWADDR), .I_CH0_AWLEN(shared_gmem3_AWLEN),
    .I_CH0_WVALID(shared_gmem3_WVALID), .I_CH0_WREADY(shared_gmem3_WREADY), .I_CH0_WDATA(shared_gmem3_WDATA), .I_CH0_WSTRB(shared_gmem3_WSTRB),
    .I_CH0_BVALID(shared_gmem3_BVALID), .I_CH0_BREADY(shared_gmem3_BREADY)
);

endmodule
