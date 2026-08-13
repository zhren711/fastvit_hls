// dwconv_only_top.v -- AUTO-EXTRACTED single-worker Tier B minimal harness.
// Isolates fastvit_ip_dwconv_worker (unmodified HLS black box) + its
// private per-bundle AXI4 adapters behind the real AXI4-Lite register
// files, for a standalone P&R timing-closure test at 200MHz. Mechanically
// derived from fastvit_top_tierb.v.FULL17_BACKUP (17-master full-independence
// Tier B attempt) -- see C:\Users\zhren\.claude\plans\typed-knitting-nygaard.md
// and gen_single_worker_tops.py (session scratchpad).
`timescale 1ns/1ps

module dwconv_only_top (
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
    output wire         m_axi_dwconv_gmem1_AWVALID,
    input  wire         m_axi_dwconv_gmem1_AWREADY,
    output wire [63:0]  m_axi_dwconv_gmem1_AWADDR,
    output wire [0:0]   m_axi_dwconv_gmem1_AWID,
    output wire [7:0]   m_axi_dwconv_gmem1_AWLEN,
    output wire [2:0]   m_axi_dwconv_gmem1_AWSIZE,
    output wire [1:0]   m_axi_dwconv_gmem1_AWBURST,
    output wire [1:0]   m_axi_dwconv_gmem1_AWLOCK,
    output wire [3:0]   m_axi_dwconv_gmem1_AWCACHE,
    output wire [2:0]   m_axi_dwconv_gmem1_AWPROT,
    output wire [3:0]   m_axi_dwconv_gmem1_AWQOS,
    output wire [3:0]   m_axi_dwconv_gmem1_AWREGION,
    output wire [0:0]   m_axi_dwconv_gmem1_AWUSER,
    output wire         m_axi_dwconv_gmem1_WVALID,
    input  wire         m_axi_dwconv_gmem1_WREADY,
    output wire [31:0]  m_axi_dwconv_gmem1_WDATA,
    output wire [3:0]   m_axi_dwconv_gmem1_WSTRB,
    output wire         m_axi_dwconv_gmem1_WLAST,
    output wire [0:0]   m_axi_dwconv_gmem1_WID,
    output wire [0:0]   m_axi_dwconv_gmem1_WUSER,
    output wire         m_axi_dwconv_gmem1_ARVALID,
    input  wire         m_axi_dwconv_gmem1_ARREADY,
    output wire [63:0]  m_axi_dwconv_gmem1_ARADDR,
    output wire [0:0]   m_axi_dwconv_gmem1_ARID,
    output wire [7:0]   m_axi_dwconv_gmem1_ARLEN,
    output wire [2:0]   m_axi_dwconv_gmem1_ARSIZE,
    output wire [1:0]   m_axi_dwconv_gmem1_ARBURST,
    output wire [1:0]   m_axi_dwconv_gmem1_ARLOCK,
    output wire [3:0]   m_axi_dwconv_gmem1_ARCACHE,
    output wire [2:0]   m_axi_dwconv_gmem1_ARPROT,
    output wire [3:0]   m_axi_dwconv_gmem1_ARQOS,
    output wire [3:0]   m_axi_dwconv_gmem1_ARREGION,
    output wire [0:0]   m_axi_dwconv_gmem1_ARUSER,
    input  wire         m_axi_dwconv_gmem1_RVALID,
    output wire         m_axi_dwconv_gmem1_RREADY,
    input  wire [31:0]  m_axi_dwconv_gmem1_RDATA,
    input  wire         m_axi_dwconv_gmem1_RLAST,
    input  wire [0:0]   m_axi_dwconv_gmem1_RID,
    input  wire [0:0]   m_axi_dwconv_gmem1_RUSER,
    input  wire [1:0]   m_axi_dwconv_gmem1_RRESP,
    input  wire         m_axi_dwconv_gmem1_BVALID,
    output wire         m_axi_dwconv_gmem1_BREADY,
    input  wire [1:0]   m_axi_dwconv_gmem1_BRESP,
    input  wire [0:0]   m_axi_dwconv_gmem1_BID,
    input  wire [0:0]   m_axi_dwconv_gmem1_BUSER,
    output wire         m_axi_dwconv_gmem2_AWVALID,
    input  wire         m_axi_dwconv_gmem2_AWREADY,
    output wire [63:0]  m_axi_dwconv_gmem2_AWADDR,
    output wire [0:0]   m_axi_dwconv_gmem2_AWID,
    output wire [7:0]   m_axi_dwconv_gmem2_AWLEN,
    output wire [2:0]   m_axi_dwconv_gmem2_AWSIZE,
    output wire [1:0]   m_axi_dwconv_gmem2_AWBURST,
    output wire [1:0]   m_axi_dwconv_gmem2_AWLOCK,
    output wire [3:0]   m_axi_dwconv_gmem2_AWCACHE,
    output wire [2:0]   m_axi_dwconv_gmem2_AWPROT,
    output wire [3:0]   m_axi_dwconv_gmem2_AWQOS,
    output wire [3:0]   m_axi_dwconv_gmem2_AWREGION,
    output wire [0:0]   m_axi_dwconv_gmem2_AWUSER,
    output wire         m_axi_dwconv_gmem2_WVALID,
    input  wire         m_axi_dwconv_gmem2_WREADY,
    output wire [31:0]  m_axi_dwconv_gmem2_WDATA,
    output wire [3:0]   m_axi_dwconv_gmem2_WSTRB,
    output wire         m_axi_dwconv_gmem2_WLAST,
    output wire [0:0]   m_axi_dwconv_gmem2_WID,
    output wire [0:0]   m_axi_dwconv_gmem2_WUSER,
    output wire         m_axi_dwconv_gmem2_ARVALID,
    input  wire         m_axi_dwconv_gmem2_ARREADY,
    output wire [63:0]  m_axi_dwconv_gmem2_ARADDR,
    output wire [0:0]   m_axi_dwconv_gmem2_ARID,
    output wire [7:0]   m_axi_dwconv_gmem2_ARLEN,
    output wire [2:0]   m_axi_dwconv_gmem2_ARSIZE,
    output wire [1:0]   m_axi_dwconv_gmem2_ARBURST,
    output wire [1:0]   m_axi_dwconv_gmem2_ARLOCK,
    output wire [3:0]   m_axi_dwconv_gmem2_ARCACHE,
    output wire [2:0]   m_axi_dwconv_gmem2_ARPROT,
    output wire [3:0]   m_axi_dwconv_gmem2_ARQOS,
    output wire [3:0]   m_axi_dwconv_gmem2_ARREGION,
    output wire [0:0]   m_axi_dwconv_gmem2_ARUSER,
    input  wire         m_axi_dwconv_gmem2_RVALID,
    output wire         m_axi_dwconv_gmem2_RREADY,
    input  wire [31:0]  m_axi_dwconv_gmem2_RDATA,
    input  wire         m_axi_dwconv_gmem2_RLAST,
    input  wire [0:0]   m_axi_dwconv_gmem2_RID,
    input  wire [0:0]   m_axi_dwconv_gmem2_RUSER,
    input  wire [1:0]   m_axi_dwconv_gmem2_RRESP,
    input  wire         m_axi_dwconv_gmem2_BVALID,
    output wire         m_axi_dwconv_gmem2_BREADY,
    input  wire [1:0]   m_axi_dwconv_gmem2_BRESP,
    input  wire [0:0]   m_axi_dwconv_gmem2_BID,
    input  wire [0:0]   m_axi_dwconv_gmem2_BUSER,
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
    input  wire [0:0]   m_axi_dwconv_gmem3_BUSER
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

reg busy_r;
always @(posedge ap_clk) begin
    if (ap_rst_n_inv) busy_r <= 1'b0;
    else if (!busy_r && ap_start_lite)        busy_r <= 1'b1;
    else if (busy_r && dwconv_worker_ap_done) busy_r <= 1'b0;
end

reg dwconv_start_hold;
always @(posedge ap_clk) begin
    if (ap_rst_n_inv) dwconv_start_hold <= 1'b0;
    else if (!busy_r && ap_start_lite)                      dwconv_start_hold <= 1'b1;
    else if (dwconv_start_hold && !dwconv_worker_ap_idle) dwconv_start_hold <= 1'b0;
end
wire dwconv_worker_ap_start = dwconv_start_hold;

assign ap_idle_o  = !busy_r;
assign ap_done_o  = busy_r & dwconv_worker_ap_done;
assign ap_ready_o = ap_done_o;

// ==================== dwconv_worker path ====================

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

fastvit_ip_gmem2_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(7), .MAX_READ_BURST_LENGTH(16), .MAX_WRITE_BURST_LENGTH(16),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(9), .CH0_USER_DW(32), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(16), .NUM_WRITE_OUTSTANDING(0))
dwconv_gmem2_m_axi_U (
    .AWVALID(m_axi_dwconv_gmem2_AWVALID), .AWREADY(m_axi_dwconv_gmem2_AWREADY), .AWADDR(m_axi_dwconv_gmem2_AWADDR),
    .AWID(m_axi_dwconv_gmem2_AWID), .AWLEN(m_axi_dwconv_gmem2_AWLEN), .AWSIZE(m_axi_dwconv_gmem2_AWSIZE),
    .AWBURST(m_axi_dwconv_gmem2_AWBURST), .AWLOCK(m_axi_dwconv_gmem2_AWLOCK), .AWCACHE(m_axi_dwconv_gmem2_AWCACHE),
    .AWPROT(m_axi_dwconv_gmem2_AWPROT), .AWQOS(m_axi_dwconv_gmem2_AWQOS), .AWREGION(m_axi_dwconv_gmem2_AWREGION), .AWUSER(m_axi_dwconv_gmem2_AWUSER),
    .WVALID(m_axi_dwconv_gmem2_WVALID), .WREADY(m_axi_dwconv_gmem2_WREADY), .WDATA(m_axi_dwconv_gmem2_WDATA),
    .WSTRB(m_axi_dwconv_gmem2_WSTRB), .WLAST(m_axi_dwconv_gmem2_WLAST), .WID(m_axi_dwconv_gmem2_WID), .WUSER(m_axi_dwconv_gmem2_WUSER),
    .ARVALID(m_axi_dwconv_gmem2_ARVALID), .ARREADY(m_axi_dwconv_gmem2_ARREADY), .ARADDR(m_axi_dwconv_gmem2_ARADDR),
    .ARID(m_axi_dwconv_gmem2_ARID), .ARLEN(m_axi_dwconv_gmem2_ARLEN), .ARSIZE(m_axi_dwconv_gmem2_ARSIZE),
    .ARBURST(m_axi_dwconv_gmem2_ARBURST), .ARLOCK(m_axi_dwconv_gmem2_ARLOCK), .ARCACHE(m_axi_dwconv_gmem2_ARCACHE),
    .ARPROT(m_axi_dwconv_gmem2_ARPROT), .ARQOS(m_axi_dwconv_gmem2_ARQOS), .ARREGION(m_axi_dwconv_gmem2_ARREGION), .ARUSER(m_axi_dwconv_gmem2_ARUSER),
    .RVALID(m_axi_dwconv_gmem2_RVALID), .RREADY(m_axi_dwconv_gmem2_RREADY), .RDATA(m_axi_dwconv_gmem2_RDATA),
    .RLAST(m_axi_dwconv_gmem2_RLAST), .RID(m_axi_dwconv_gmem2_RID), .RUSER(m_axi_dwconv_gmem2_RUSER), .RRESP(m_axi_dwconv_gmem2_RRESP),
    .BVALID(m_axi_dwconv_gmem2_BVALID), .BREADY(m_axi_dwconv_gmem2_BREADY), .BRESP(m_axi_dwconv_gmem2_BRESP),
    .BID(m_axi_dwconv_gmem2_BID), .BUSER(m_axi_dwconv_gmem2_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(dwconv_g2_ARVALID), .I_CH0_ARREADY(dwconv_g2_ARREADY_i), .I_CH0_ARADDR(dwconv_g2_ARADDR), .I_CH0_ARLEN(dwconv_g2_ARLEN),
    .I_CH0_RVALID(dwconv_g2_RVALID_i), .I_CH0_RREADY(dwconv_g2_RREADY), .I_CH0_RDATA(dwconv_g2_RDATA_i), .I_CH0_RFIFONUM(dwconv_g2_RFIFONUM_i),
    .I_CH0_AWVALID(1'b0), .I_CH0_AWREADY(dwconv_g2_AWREADY_i), .I_CH0_AWADDR(64'd0), .I_CH0_AWLEN(32'd0),
    .I_CH0_WVALID(1'b0), .I_CH0_WREADY(dwconv_g2_WREADY_i), .I_CH0_WDATA(32'd0), .I_CH0_WSTRB(4'd0),
    .I_CH0_BVALID(dwconv_g2_BVALID_i), .I_CH0_BREADY(1'b0)
);

fastvit_ip_gmem3_m_axi #(
    .CONSERVATIVE(1), .USER_MAXREQS(67), .MAX_READ_BURST_LENGTH(16), .MAX_WRITE_BURST_LENGTH(256),
    .C_M_AXI_ID_WIDTH(1), .C_M_AXI_ADDR_WIDTH(64), .C_M_AXI_DATA_WIDTH(32),
    .C_M_AXI_AWUSER_WIDTH(1), .C_M_AXI_ARUSER_WIDTH(1), .C_M_AXI_WUSER_WIDTH(1),
    .C_M_AXI_RUSER_WIDTH(1), .C_M_AXI_BUSER_WIDTH(1),
    .C_USER_VALUE(0), .C_PROT_VALUE(0), .C_CACHE_VALUE(3),
    .CH0_USER_RFIFONUM_WIDTH(9), .CH0_USER_DW(32), .CH0_USER_AW(64),
    .NUM_READ_OUTSTANDING(0), .NUM_WRITE_OUTSTANDING(4))
dwconv_gmem3_m_axi_U (
    .AWVALID(m_axi_dwconv_gmem3_AWVALID), .AWREADY(m_axi_dwconv_gmem3_AWREADY), .AWADDR(m_axi_dwconv_gmem3_AWADDR),
    .AWID(m_axi_dwconv_gmem3_AWID), .AWLEN(m_axi_dwconv_gmem3_AWLEN), .AWSIZE(m_axi_dwconv_gmem3_AWSIZE),
    .AWBURST(m_axi_dwconv_gmem3_AWBURST), .AWLOCK(m_axi_dwconv_gmem3_AWLOCK), .AWCACHE(m_axi_dwconv_gmem3_AWCACHE),
    .AWPROT(m_axi_dwconv_gmem3_AWPROT), .AWQOS(m_axi_dwconv_gmem3_AWQOS), .AWREGION(m_axi_dwconv_gmem3_AWREGION), .AWUSER(m_axi_dwconv_gmem3_AWUSER),
    .WVALID(m_axi_dwconv_gmem3_WVALID), .WREADY(m_axi_dwconv_gmem3_WREADY), .WDATA(m_axi_dwconv_gmem3_WDATA),
    .WSTRB(m_axi_dwconv_gmem3_WSTRB), .WLAST(m_axi_dwconv_gmem3_WLAST), .WID(m_axi_dwconv_gmem3_WID), .WUSER(m_axi_dwconv_gmem3_WUSER),
    .ARVALID(m_axi_dwconv_gmem3_ARVALID), .ARREADY(m_axi_dwconv_gmem3_ARREADY), .ARADDR(m_axi_dwconv_gmem3_ARADDR),
    .ARID(m_axi_dwconv_gmem3_ARID), .ARLEN(m_axi_dwconv_gmem3_ARLEN), .ARSIZE(m_axi_dwconv_gmem3_ARSIZE),
    .ARBURST(m_axi_dwconv_gmem3_ARBURST), .ARLOCK(m_axi_dwconv_gmem3_ARLOCK), .ARCACHE(m_axi_dwconv_gmem3_ARCACHE),
    .ARPROT(m_axi_dwconv_gmem3_ARPROT), .ARQOS(m_axi_dwconv_gmem3_ARQOS), .ARREGION(m_axi_dwconv_gmem3_ARREGION), .ARUSER(m_axi_dwconv_gmem3_ARUSER),
    .RVALID(m_axi_dwconv_gmem3_RVALID), .RREADY(m_axi_dwconv_gmem3_RREADY), .RDATA(m_axi_dwconv_gmem3_RDATA),
    .RLAST(m_axi_dwconv_gmem3_RLAST), .RID(m_axi_dwconv_gmem3_RID), .RUSER(m_axi_dwconv_gmem3_RUSER), .RRESP(m_axi_dwconv_gmem3_RRESP),
    .BVALID(m_axi_dwconv_gmem3_BVALID), .BREADY(m_axi_dwconv_gmem3_BREADY), .BRESP(m_axi_dwconv_gmem3_BRESP),
    .BID(m_axi_dwconv_gmem3_BID), .BUSER(m_axi_dwconv_gmem3_BUSER),
    .ACLK(ap_clk), .ARESET(ap_rst_n_inv), .ACLK_EN(1'b1),
    .I_CH0_ARVALID(1'b0), .I_CH0_ARREADY(dwconv_g3_ARREADY_i), .I_CH0_ARADDR(64'd0), .I_CH0_ARLEN(32'd0),
    .I_CH0_RVALID(dwconv_g3_RVALID_i), .I_CH0_RREADY(1'b0), .I_CH0_RDATA(dwconv_g3_RDATA_i), .I_CH0_RFIFONUM(dwconv_g3_RFIFONUM_i),
    .I_CH0_AWVALID(dwconv_g3_AWVALID), .I_CH0_AWREADY(dwconv_g3_AWREADY_i), .I_CH0_AWADDR(dwconv_g3_AWADDR), .I_CH0_AWLEN(dwconv_g3_AWLEN),
    .I_CH0_WVALID(dwconv_g3_WVALID), .I_CH0_WREADY(dwconv_g3_WREADY_i), .I_CH0_WDATA(dwconv_g3_WDATA), .I_CH0_WSTRB(dwconv_g3_WSTRB),
    .I_CH0_BVALID(dwconv_g3_BVALID_i), .I_CH0_BREADY(dwconv_g3_BREADY)
);

endmodule
