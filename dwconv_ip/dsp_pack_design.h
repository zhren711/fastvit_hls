#ifndef __DSP_PACK_DESIGN_H__
#define __DSP_PACK_DESIGN_H__
#include <ap_int.h>

typedef ap_int<8>  act_t;
typedef ap_int<8>  wt_t;
typedef ap_int<32> acc_t;

void dsp_packed_mac2_top(act_t fi0, act_t fi1, wt_t w, acc_t &out0, acc_t &out1);

#endif
