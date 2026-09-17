// cdc_probe_tb.sv -- ZHR-92 (2026-09-17), (b) step 1: per-transaction latency of an AXI VIP master
// through SmartConnect / AXI Interconnect into an AXI BRAM controller, same-clock vs async clocks.
// Prints CDC_RESULT lines: the blocking-call duration (AW/AR issue -> B/RLAST received) per
// transaction shape, in ns and in master-clock cycles. Driven by run_cdc_probe.tcl.
`timescale 1ns/1ps
import axi_vip_pkg::*;
import cdc_bd_axi_vip_0_0_pkg::*;

module cdc_probe_tb;
  // clock periods come in as plusargs at RUN time (a parameter/generic override was found to leave
  // the `always #(...)` delay at its default in XSim while $display showed the new value)
  real MPER = 10.0;
  real SPER = 10.0;

  bit mclk = 0, sclk = 0, aresetn = 0;
  initial begin
    if (!$value$plusargs("MPER=%f", MPER)) MPER = 10.0;
    if (!$value$plusargs("SPER=%f", SPER)) SPER = 10.0;
  end
  always begin #(MPER/2.0) mclk = ~mclk; end
  always begin #(SPER/2.0) sclk = ~sclk; end

  cdc_bd_wrapper DUT (.mclk(mclk), .sclk(sclk), .aresetn(aresetn));

  cdc_bd_axi_vip_0_0_mst_t agent;

  xil_axi_resp_t                resp;
  xil_axi_resp_t [255:0]        rresp;
  bit [8*4096-1:0]              wdata;
  bit [8*4096-1:0]              rdata;
  bit [63:0]                    ldata;
  xil_axi_data_beat [255:0]     wuser;
  xil_axi_data_beat [255:0]     ruser;
  real                  t0, t1;
  longint               cnt_m = 0, cnt_s = 0;
  real                  t_m_aw = -1, t_s_aw = -1, t_s_b = -1, t_m_b = -1;
  always @(posedge mclk) cnt_m++;
  always @(posedge DUT.cdc_bd_i.axi_bram_ctrl_0.s_axi_aclk) cnt_s++;
  // one-shot timestamps of the first write after warm-up: AW at master, AW at slave, B at slave, B at master
  always @(posedge mclk) if (cnt_m > 400 && t_m_aw < 0 && DUT.cdc_bd_i.axi_vip_0.m_axi_awvalid && DUT.cdc_bd_i.axi_vip_0.m_axi_awready) t_m_aw = $realtime;
  always @(posedge DUT.cdc_bd_i.axi_bram_ctrl_0.s_axi_aclk) if (t_m_aw >= 0 && t_s_aw < 0 && DUT.cdc_bd_i.axi_bram_ctrl_0.s_axi_awvalid && DUT.cdc_bd_i.axi_bram_ctrl_0.s_axi_awready) t_s_aw = $realtime;
  always @(posedge DUT.cdc_bd_i.axi_bram_ctrl_0.s_axi_aclk) if (t_s_aw >= 0 && t_s_b < 0 && DUT.cdc_bd_i.axi_bram_ctrl_0.s_axi_bvalid && DUT.cdc_bd_i.axi_bram_ctrl_0.s_axi_bready) t_s_b = $realtime;
  always @(posedge mclk) if (t_s_b >= 0 && t_m_b < 0 && DUT.cdc_bd_i.axi_vip_0.m_axi_bvalid && DUT.cdc_bd_i.axi_vip_0.m_axi_bready) t_m_b = $realtime;

  task automatic measure(input string tag, input int nbeats, input int is_write, input int reps);
    real acc = 0.0;
    for (int r = 0; r < reps; r++) begin
      t0 = $realtime;
`ifdef CDC_PROTO_AXI4LITE
      if (is_write) agent.AXI4LITE_WRITE_BURST(32'h0000_0100 + 4*r, 0, wdata[63:0], resp);
      else          agent.AXI4LITE_READ_BURST (32'h0000_0100 + 4*r, 0, ldata, resp);
`else
      if (is_write) agent.AXI4_WRITE_BURST(0, 32'h0000_0100 + 64*r, nbeats-1, XIL_AXI_SIZE_4BYTE, XIL_AXI_BURST_TYPE_INCR,
                                           0, 0, 0, 0, 0, 0, wdata, wuser, resp);
      else          agent.AXI4_READ_BURST (0, 32'h0000_0100 + 64*r, nbeats-1, XIL_AXI_SIZE_4BYTE, XIL_AXI_BURST_TYPE_INCR,
                                           0, 0, 0, 0, 0, 0, rdata, rresp, ruser);
`endif
      t1 = $realtime;
      acc += (t1 - t0);
    end
    $display("CDC_RESULT %-10s beats=%0d write=%0d  mean=%8.2f ns = %6.2f mclk cycles (MPER=%0.3f SPER=%0.3f)",
             tag, nbeats, is_write, acc/reps, (acc/reps)/MPER, MPER, SPER);
  endtask

  initial begin
    wdata = {1024{32'hA5A5_0001}};
    wuser = '{default:0};
    aresetn = 0;
    repeat (20) @(posedge mclk);
    aresetn = 1;
    repeat (20) @(posedge mclk);
    agent = new("mst", DUT.cdc_bd_i.axi_vip_0.inst.IF);
    agent.start_master();
    repeat (10) @(posedge mclk);
    // warm-up (first transaction pays any one-time cost)
    measure("warmup",  1, 1, 2);
    measure("write1",  1, 1, 16);
    measure("read1",   1, 0, 16);
`ifndef CDC_PROTO_AXI4LITE
    measure("write4",  4, 1, 16);
    measure("read4",   4, 0, 16);
    measure("write16", 16, 1, 16);
    measure("read16",  16, 0, 16);
`endif
    $display("CDC_RESULT clocks: mclk edges=%0d sclk edges=%0d (ratio %.3f)", cnt_m, cnt_s, real'(cnt_m)/real'(cnt_s));
    $display("CDC_RESULT trace: AW@master=%.1f AW@slave=%.1f B@slave=%.1f B@master=%.1f  -> AW crossing %.1f ns, B crossing %.1f ns", t_m_aw, t_s_aw, t_s_b, t_m_b, t_s_aw-t_m_aw, t_m_b-t_s_b);
    $display("CDC_RESULT done");
    $finish;
  end
endmodule
