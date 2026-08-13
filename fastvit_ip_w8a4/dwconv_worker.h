#ifndef __DWCONV_WORKER_H__
#define __DWCONV_WORKER_H__

#include "fastvit_ip.h"

/* 2026-08-11: swapped in the line-buffer/shift-register rewrite
 * (fastvit_ip_w8a4/dwconv_linebuf/dwconv_worker.cpp, isolated WNS
 * -0.236ns after pos_t narrowing) in place of the monolithic tile
 * design (backed up as dwconv_worker.tile_backup_2530ns.cpp/.h --
 * that version reproduces this project's -2.530ns Tier A combined
 * baseline; restore from there if this integration needs reverting).
 * fastvit_ip.h's act_t/wt_t/acc_t/pack_t typedefs and DW_MAX_K/CH/H/W
 * macros are identical to dwconv_linebuf's own standalone header, so
 * this is a signature- and type-compatible drop-in replacement --
 * fastvit_ip.cpp itself needs no changes. */
void dwconv_worker(
    pack_t feat_in[],
    wt_t   weight[],
    acc_t  bias[],
    pack_t feat_out[],
    int    CHin,
    int    Hin,
    int    Win,
    int    Kh,
    int    Kw,
    int    stride_h,
    int    stride_w,
    int    pad_h,
    int    pad_w,
    int    fpg,
    int    act_mode,
    int    out_shift
);

#endif // __DWCONV_WORKER_H__
