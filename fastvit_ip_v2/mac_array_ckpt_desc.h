#pragma once
#include "mac_array.h"

static const int N_HW_SEQ = 82;
static LayerDescV2 g_hw_seq[N_HW_SEQ] = {
    { 7, 48,48, 128,128, 1,1,0, 1, 7, 0,0,0,786432, 0 }, // [0] gelu
    { 0, 48,48, 128,128, 3,2,1, 1, 0, 786432,0,0,0, 0 }, // [1] conv layer_idx=1 tag=layer_0001_dwconv
    { 7, 48,48, 64,64, 1,1,0, 1, 7, 0,0,0,786432, 0 }, // [2] gelu
    { 1, 48,48, 64,64, 1,1,0, 1, 0, 786432,432,48,0, 0 }, // [3] conv layer_idx=2 tag=layer_0002_pwconv
    { 7, 48,48, 64,64, 1,1,0, 1, 7, 0,0,0,786432, 0 }, // [4] gelu
    { 0, 48,48, 64,64, 3,1,1, 1, 0, 786432,2736,96,1572864, 0 }, // [5] conv layer_idx=3 tag=layer_0003_dwconv
    { 0, 48,48, 64,64, 7,1,3, 1, 0, 1572864,3168,144,0, 0 }, // [6] conv layer_idx=4 tag=layer_0004_dwconv
    { 1, 48,144, 64,64, 1,1,0, 1, 0, 0,5520,192,786432, 0 }, // [7] conv layer_idx=5 tag=layer_0005_pwconv
    { 7, 144,144, 64,64, 1,1,0, 1, 7, 786432,0,0,0, 0 }, // [8] gelu
    { 1, 144,48, 64,64, 1,1,0, 1, 0, 0,12432,336,786432, 0 }, // [9] conv layer_idx=6 tag=layer_0006_pwconv
    { 2, 48,48, 64,64, 1,1,0, 1, 0, 1572864,0,0,0, 786432 }, // [10] add
    { 0, 48,48, 64,64, 3,1,1, 1, 0, 0,19344,384,1572864, 0 }, // [11] conv layer_idx=7 tag=layer_0007_dwconv
    { 0, 48,48, 64,64, 7,1,3, 1, 0, 1572864,19776,432,786432, 0 }, // [12] conv layer_idx=8 tag=layer_0008_dwconv
    { 1, 48,144, 64,64, 1,1,0, 1, 0, 786432,22128,480,0, 0 }, // [13] conv layer_idx=9 tag=layer_0009_pwconv
    { 7, 144,144, 64,64, 1,1,0, 1, 7, 0,0,0,786432, 0 }, // [14] gelu
    { 1, 144,48, 64,64, 1,1,0, 1, 0, 786432,29040,624,0, 0 }, // [15] conv layer_idx=10 tag=layer_0010_pwconv
    { 2, 48,48, 64,64, 1,1,0, 1, 0, 1572864,0,0,786432, 0 }, // [16] add
    { 0, 48,96, 64,64, 7,2,3, 2, 0, 786432,35952,672,0, 0 }, // [17] conv layer_idx=11 tag=layer_0011_dwconv
    { 1, 96,96, 32,32, 1,1,0, 1, 0, 0,40656,768,786432, 0 }, // [18] conv layer_idx=12 tag=layer_0012_pwconv
    { 7, 96,96, 32,32, 1,1,0, 1, 7, 786432,0,0,0, 0 }, // [19] gelu
    { 0, 96,96, 32,32, 3,1,1, 1, 0, 0,49872,864,1572864, 0 }, // [20] conv layer_idx=13 tag=layer_0013_dwconv
    { 0, 96,96, 32,32, 7,1,3, 1, 0, 1572864,50736,960,786432, 0 }, // [21] conv layer_idx=14 tag=layer_0014_dwconv
    { 1, 96,240, 32,32, 1,1,0, 1, 0, 786432,55440,1056,0, 0 }, // [22] conv layer_idx=15 tag=layer_0015_pwconv
    { 7, 240,240, 32,32, 1,1,0, 1, 7, 0,0,0,786432, 0 }, // [23] gelu
    { 1, 240,96, 32,32, 1,1,0, 1, 0, 786432,78480,1296,0, 0 }, // [24] conv layer_idx=16 tag=layer_0016_pwconv
    { 2, 96,96, 32,32, 1,1,0, 1, 0, 1572864,0,0,786432, 0 }, // [25] add
    { 0, 96,96, 32,32, 3,1,1, 1, 0, 786432,101520,1392,1572864, 0 }, // [26] conv layer_idx=17 tag=layer_0017_dwconv
    { 0, 96,96, 32,32, 7,1,3, 1, 0, 1572864,102384,1488,0, 0 }, // [27] conv layer_idx=18 tag=layer_0018_dwconv
    { 1, 96,288, 32,32, 1,1,0, 1, 0, 0,107088,1584,786432, 0 }, // [28] conv layer_idx=19 tag=layer_0019_pwconv
    { 7, 288,288, 32,32, 1,1,0, 1, 7, 786432,0,0,0, 0 }, // [29] gelu
    { 1, 288,96, 32,32, 1,1,0, 1, 0, 0,134736,1872,786432, 0 }, // [30] conv layer_idx=20 tag=layer_0020_pwconv
    { 2, 96,96, 32,32, 1,1,0, 1, 0, 1572864,0,0,0, 786432 }, // [31] add
    { 0, 96,192, 32,32, 7,2,3, 2, 0, 0,162384,1968,786432, 0 }, // [32] conv layer_idx=21 tag=layer_0021_dwconv
    { 1, 192,192, 16,16, 1,1,0, 1, 0, 786432,171792,2160,0, 0 }, // [33] conv layer_idx=22 tag=layer_0022_pwconv
    { 7, 192,192, 16,16, 1,1,0, 1, 7, 0,0,0,786432, 0 }, // [34] gelu
    { 0, 192,192, 16,16, 3,1,1, 1, 0, 786432,208656,2352,1572864, 0 }, // [35] conv layer_idx=23 tag=layer_0023_dwconv
    { 0, 192,192, 16,16, 7,1,3, 1, 0, 1572864,210384,2544,0, 0 }, // [36] conv layer_idx=24 tag=layer_0024_dwconv
    { 1, 192,576, 16,16, 1,1,0, 1, 0, 0,219792,2736,786432, 0 }, // [37] conv layer_idx=25 tag=layer_0025_pwconv
    { 7, 576,576, 16,16, 1,1,0, 1, 7, 786432,0,0,0, 0 }, // [38] gelu
    { 1, 576,192, 16,16, 1,1,0, 1, 0, 0,330384,3312,786432, 0 }, // [39] conv layer_idx=26 tag=layer_0026_pwconv
    { 2, 192,192, 16,16, 1,1,0, 1, 0, 1572864,0,0,0, 786432 }, // [40] add
    { 0, 192,192, 16,16, 3,1,1, 1, 0, 0,440976,3504,1572864, 0 }, // [41] conv layer_idx=27 tag=layer_0027_dwconv
    { 0, 192,192, 16,16, 7,1,3, 1, 0, 1572864,442704,3696,786432, 0 }, // [42] conv layer_idx=28 tag=layer_0028_dwconv
    { 1, 192,480, 16,16, 1,1,0, 1, 0, 786432,452112,3888,0, 0 }, // [43] conv layer_idx=29 tag=layer_0029_pwconv
    { 7, 480,480, 16,16, 1,1,0, 1, 7, 0,0,0,786432, 0 }, // [44] gelu
    { 1, 480,192, 16,16, 1,1,0, 1, 0, 786432,544272,4368,0, 0 }, // [45] conv layer_idx=30 tag=layer_0030_pwconv
    { 2, 192,192, 16,16, 1,1,0, 1, 0, 1572864,0,0,786432, 0 }, // [46] add
    { 0, 192,192, 16,16, 3,1,1, 1, 0, 786432,636432,4560,1572864, 0 }, // [47] conv layer_idx=31 tag=layer_0031_dwconv
    { 0, 192,192, 16,16, 7,1,3, 1, 0, 1572864,638160,4752,0, 0 }, // [48] conv layer_idx=32 tag=layer_0032_dwconv
    { 1, 192,576, 16,16, 1,1,0, 1, 0, 0,647568,4944,786432, 0 }, // [49] conv layer_idx=33 tag=layer_0033_pwconv
    { 7, 576,576, 16,16, 1,1,0, 1, 7, 786432,0,0,0, 0 }, // [50] gelu
    { 1, 576,192, 16,16, 1,1,0, 1, 0, 0,758160,5520,786432, 0 }, // [51] conv layer_idx=34 tag=layer_0034_pwconv
    { 2, 192,192, 16,16, 1,1,0, 1, 0, 1572864,0,0,0, 786432 }, // [52] add
    { 0, 192,192, 16,16, 3,1,1, 1, 0, 0,868752,5712,1572864, 0 }, // [53] conv layer_idx=35 tag=layer_0035_dwconv
    { 0, 192,192, 16,16, 7,1,3, 1, 0, 1572864,870480,5904,786432, 0 }, // [54] conv layer_idx=36 tag=layer_0036_dwconv
    { 1, 192,576, 16,16, 1,1,0, 1, 0, 786432,879888,6096,0, 0 }, // [55] conv layer_idx=37 tag=layer_0037_pwconv
    { 7, 576,576, 16,16, 1,1,0, 1, 7, 0,0,0,786432, 0 }, // [56] gelu
    { 1, 576,192, 16,16, 1,1,0, 1, 0, 786432,990480,6672,0, 0 }, // [57] conv layer_idx=38 tag=layer_0038_pwconv
    { 2, 192,192, 16,16, 1,1,0, 1, 0, 1572864,0,0,786432, 0 }, // [58] add
    { 0, 192,384, 16,16, 7,2,3, 2, 0, 786432,1101072,6864,0, 0 }, // [59] conv layer_idx=39 tag=layer_0039_dwconv
    { 1, 384,384, 8,8, 1,1,0, 1, 0, 0,1119888,7248,786432, 0 }, // [60] conv layer_idx=40 tag=layer_0040_pwconv
    { 7, 384,384, 8,8, 1,1,0, 1, 7, 786432,0,0,0, 0 }, // [61] gelu
    { 0, 384,384, 8,8, 3,1,1, 1, 0, 0,1267344,7632,1572864, 0 }, // [62] conv layer_idx=41 tag=layer_0041_dwconv
    { 0, 384,384, 8,8, 7,1,3, 1, 0, 1572864,1270800,8016,786432, 0 }, // [63] conv layer_idx=42 tag=layer_0042_dwconv
    { 1, 384,1152, 8,8, 1,1,0, 1, 0, 786432,1289616,8400,0, 0 }, // [64] conv layer_idx=43 tag=layer_0043_pwconv
    { 7, 1152,1152, 8,8, 1,1,0, 1, 7, 0,0,0,786432, 0 }, // [65] gelu
    { 1, 1152,384, 8,8, 1,1,0, 1, 0, 786432,1731984,9552,0, 0 }, // [66] conv layer_idx=44 tag=layer_0044_pwconv
    { 2, 384,384, 8,8, 1,1,0, 1, 0, 1572864,0,0,786432, 0 }, // [67] add
    { 0, 384,384, 8,8, 3,1,1, 1, 0, 786432,2174352,9936,1572864, 0 }, // [68] conv layer_idx=45 tag=layer_0045_dwconv
    { 0, 384,384, 8,8, 7,1,3, 1, 0, 1572864,2177808,10320,0, 0 }, // [69] conv layer_idx=46 tag=layer_0046_dwconv
    { 1, 384,960, 8,8, 1,1,0, 1, 0, 0,2196624,10704,786432, 0 }, // [70] conv layer_idx=47 tag=layer_0047_pwconv
    { 7, 960,960, 8,8, 1,1,0, 1, 7, 786432,0,0,0, 0 }, // [71] gelu
    { 1, 960,384, 8,8, 1,1,0, 1, 0, 0,2565264,11664,786432, 0 }, // [72] conv layer_idx=48 tag=layer_0048_pwconv
    { 2, 384,384, 8,8, 1,1,0, 1, 0, 1572864,0,0,0, 786432 }, // [73] add
    { 0, 384,768, 8,8, 3,1,1, 2, 0, 0,2933904,12048,1769472, 0 }, // [74] conv layer_idx=49 tag=layer_0049_dwconv
    { 3, 768,768, 8,8, 1,1,0, 1, 0, 1769472,0,0,1818624, 0 }, // [75] gap
    { 1, 768,48, 1,1, 1,1,0, 1, 0, 1818624,2940816,12816,786432, 0 }, // [76] conv layer_idx=50 tag=layer_0050_pwconv
    { 4, 48,48, 1,1, 1,1,0, 1, 0, 786432,0,0,0, 0 }, // [77] relu
    { 1, 48,768, 1,1, 1,1,0, 1, 0, 0,2977680,12864,786432, 0 }, // [78] conv layer_idx=51 tag=layer_0051_pwconv
    { 5, 768,768, 1,1, 1,1,0, 1, 0, 786432,0,0,0, 0 }, // [79] sigmoid
    { 6, 768,768, 8,8, 1,1,0, 1, 7, 1769472,0,0,786432, 0 }, // [80] scale
    { 7, 768,768, 8,8, 1,1,0, 1, 7, 786432,0,0,0, 0 }, // [81] gelu
};

// use_shift_table/shift_off sit 8 fields after in2_off in the struct
// (past all the host-computed h_out/tile fields) -- positional init can't
// reach them without also fixing every field in between, so set by name,
// same convention this codebase already uses for in2_off elsewhere.
static void set_shift_table_fields() {
    g_hw_seq[0].use_shift_table = 0; g_hw_seq[0].shift_off = 0;
    g_hw_seq[1].use_shift_table = 1; g_hw_seq[1].shift_off = 3014544;
    g_hw_seq[2].use_shift_table = 0; g_hw_seq[2].shift_off = 0;
    g_hw_seq[3].use_shift_table = 1; g_hw_seq[3].shift_off = 3014592;
    g_hw_seq[4].use_shift_table = 0; g_hw_seq[4].shift_off = 0;
    g_hw_seq[5].use_shift_table = 1; g_hw_seq[5].shift_off = 3014640;
    g_hw_seq[6].use_shift_table = 1; g_hw_seq[6].shift_off = 3014688;
    g_hw_seq[7].use_shift_table = 1; g_hw_seq[7].shift_off = 3014736;
    g_hw_seq[8].use_shift_table = 0; g_hw_seq[8].shift_off = 0;
    g_hw_seq[9].use_shift_table = 1; g_hw_seq[9].shift_off = 3014880;
    g_hw_seq[10].use_shift_table = 0; g_hw_seq[10].shift_off = 0;
    g_hw_seq[11].use_shift_table = 1; g_hw_seq[11].shift_off = 3014928;
    g_hw_seq[12].use_shift_table = 1; g_hw_seq[12].shift_off = 3014976;
    g_hw_seq[13].use_shift_table = 1; g_hw_seq[13].shift_off = 3015024;
    g_hw_seq[14].use_shift_table = 0; g_hw_seq[14].shift_off = 0;
    g_hw_seq[15].use_shift_table = 1; g_hw_seq[15].shift_off = 3015168;
    g_hw_seq[16].use_shift_table = 0; g_hw_seq[16].shift_off = 0;
    g_hw_seq[17].use_shift_table = 1; g_hw_seq[17].shift_off = 3015216;
    g_hw_seq[18].use_shift_table = 1; g_hw_seq[18].shift_off = 3015312;
    g_hw_seq[19].use_shift_table = 0; g_hw_seq[19].shift_off = 0;
    g_hw_seq[20].use_shift_table = 1; g_hw_seq[20].shift_off = 3015408;
    g_hw_seq[21].use_shift_table = 1; g_hw_seq[21].shift_off = 3015504;
    g_hw_seq[22].use_shift_table = 1; g_hw_seq[22].shift_off = 3015600;
    g_hw_seq[23].use_shift_table = 0; g_hw_seq[23].shift_off = 0;
    g_hw_seq[24].use_shift_table = 1; g_hw_seq[24].shift_off = 3015840;
    g_hw_seq[25].use_shift_table = 0; g_hw_seq[25].shift_off = 0;
    g_hw_seq[26].use_shift_table = 1; g_hw_seq[26].shift_off = 3015936;
    g_hw_seq[27].use_shift_table = 1; g_hw_seq[27].shift_off = 3016032;
    g_hw_seq[28].use_shift_table = 1; g_hw_seq[28].shift_off = 3016128;
    g_hw_seq[29].use_shift_table = 0; g_hw_seq[29].shift_off = 0;
    g_hw_seq[30].use_shift_table = 1; g_hw_seq[30].shift_off = 3016416;
    g_hw_seq[31].use_shift_table = 0; g_hw_seq[31].shift_off = 0;
    g_hw_seq[32].use_shift_table = 1; g_hw_seq[32].shift_off = 3016512;
    g_hw_seq[33].use_shift_table = 1; g_hw_seq[33].shift_off = 3016704;
    g_hw_seq[34].use_shift_table = 0; g_hw_seq[34].shift_off = 0;
    g_hw_seq[35].use_shift_table = 1; g_hw_seq[35].shift_off = 3016896;
    g_hw_seq[36].use_shift_table = 1; g_hw_seq[36].shift_off = 3017088;
    g_hw_seq[37].use_shift_table = 1; g_hw_seq[37].shift_off = 3017280;
    g_hw_seq[38].use_shift_table = 0; g_hw_seq[38].shift_off = 0;
    g_hw_seq[39].use_shift_table = 1; g_hw_seq[39].shift_off = 3017856;
    g_hw_seq[40].use_shift_table = 0; g_hw_seq[40].shift_off = 0;
    g_hw_seq[41].use_shift_table = 1; g_hw_seq[41].shift_off = 3018048;
    g_hw_seq[42].use_shift_table = 1; g_hw_seq[42].shift_off = 3018240;
    g_hw_seq[43].use_shift_table = 1; g_hw_seq[43].shift_off = 3018432;
    g_hw_seq[44].use_shift_table = 0; g_hw_seq[44].shift_off = 0;
    g_hw_seq[45].use_shift_table = 1; g_hw_seq[45].shift_off = 3018912;
    g_hw_seq[46].use_shift_table = 0; g_hw_seq[46].shift_off = 0;
    g_hw_seq[47].use_shift_table = 1; g_hw_seq[47].shift_off = 3019104;
    g_hw_seq[48].use_shift_table = 1; g_hw_seq[48].shift_off = 3019296;
    g_hw_seq[49].use_shift_table = 1; g_hw_seq[49].shift_off = 3019488;
    g_hw_seq[50].use_shift_table = 0; g_hw_seq[50].shift_off = 0;
    g_hw_seq[51].use_shift_table = 1; g_hw_seq[51].shift_off = 3020064;
    g_hw_seq[52].use_shift_table = 0; g_hw_seq[52].shift_off = 0;
    g_hw_seq[53].use_shift_table = 1; g_hw_seq[53].shift_off = 3020256;
    g_hw_seq[54].use_shift_table = 1; g_hw_seq[54].shift_off = 3020448;
    g_hw_seq[55].use_shift_table = 1; g_hw_seq[55].shift_off = 3020640;
    g_hw_seq[56].use_shift_table = 0; g_hw_seq[56].shift_off = 0;
    g_hw_seq[57].use_shift_table = 1; g_hw_seq[57].shift_off = 3021216;
    g_hw_seq[58].use_shift_table = 0; g_hw_seq[58].shift_off = 0;
    g_hw_seq[59].use_shift_table = 1; g_hw_seq[59].shift_off = 3021408;
    g_hw_seq[60].use_shift_table = 1; g_hw_seq[60].shift_off = 3021792;
    g_hw_seq[61].use_shift_table = 0; g_hw_seq[61].shift_off = 0;
    g_hw_seq[62].use_shift_table = 1; g_hw_seq[62].shift_off = 3022176;
    g_hw_seq[63].use_shift_table = 1; g_hw_seq[63].shift_off = 3022560;
    g_hw_seq[64].use_shift_table = 1; g_hw_seq[64].shift_off = 3022944;
    g_hw_seq[65].use_shift_table = 0; g_hw_seq[65].shift_off = 0;
    g_hw_seq[66].use_shift_table = 1; g_hw_seq[66].shift_off = 3024096;
    g_hw_seq[67].use_shift_table = 0; g_hw_seq[67].shift_off = 0;
    g_hw_seq[68].use_shift_table = 1; g_hw_seq[68].shift_off = 3024480;
    g_hw_seq[69].use_shift_table = 1; g_hw_seq[69].shift_off = 3024864;
    g_hw_seq[70].use_shift_table = 1; g_hw_seq[70].shift_off = 3025248;
    g_hw_seq[71].use_shift_table = 0; g_hw_seq[71].shift_off = 0;
    g_hw_seq[72].use_shift_table = 1; g_hw_seq[72].shift_off = 3026208;
    g_hw_seq[73].use_shift_table = 0; g_hw_seq[73].shift_off = 0;
    g_hw_seq[74].use_shift_table = 1; g_hw_seq[74].shift_off = 3026592;
    g_hw_seq[75].use_shift_table = 0; g_hw_seq[75].shift_off = 0;
    g_hw_seq[76].use_shift_table = 1; g_hw_seq[76].shift_off = 3027360;
    g_hw_seq[77].use_shift_table = 0; g_hw_seq[77].shift_off = 0;
    g_hw_seq[78].use_shift_table = 1; g_hw_seq[78].shift_off = 3027408;
    g_hw_seq[79].use_shift_table = 0; g_hw_seq[79].shift_off = 0;
    g_hw_seq[80].use_shift_table = 0; g_hw_seq[80].shift_off = 0;
    g_hw_seq[81].use_shift_table = 0; g_hw_seq[81].shift_off = 0;
}

struct CkptEntry { const char* tag; int seq_index; int out_off; int size; };
static const int N_CKPT = 6;
static CkptEntry g_ckpts[N_CKPT] = {
    { "stage1", 16, 786432, 196608 },
    { "stage2", 31, 0, 98304 },
    { "stage3", 58, 786432, 49152 },
    { "stage4", 73, 0, 24576 },
    { "finaldw", 74, 1769472, 49152 },
    { "se", 80, 786432, 49152 },
};

