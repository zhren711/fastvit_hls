/*============================================================
 * tb_add_ip.cpp
 * add_ip Testbench
 * 测试用例:
 *   1. 正常加法 (无溢出)
 *   2. 正向溢出 (+)
 *   3. 负向溢出 (-)
 *   4. 非整除tile尺寸
 *   5. 单通道
 *============================================================*/

#include "add_ip.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <ctime>

using namespace std;

// 简单的参考实现 (带饱和)
void ref_add(act_t* in1, act_t* in2, act_t* out, int size) {
    for (int i = 0; i < size; i++) {
        ap_int<16> sum = (ap_int<16>)in1[i] + (ap_int<16>)in2[i];
        if (sum > 127) out[i] = 127;
        else if (sum < -128) out[i] = -128;
        else out[i] = (act_t)sum;
    }
}

// 比较结果
bool check_result(act_t* hw, act_t* ref, int size, const char* test_name) {
    bool pass = true;
    for (int i = 0; i < size; i++) {
        if (hw[i] != ref[i]) {
            cout << "[FAIL] " << test_name << " mismatch at idx " << i
                 << ": hw=" << (int)hw[i] << ", ref=" << (int)ref[i] << endl;
            pass = false;
            break;
        }
    }
    if (pass) {
        cout << "[PASS] " << test_name << endl;
    }
    return pass;
}

int main() {
    const int MAX_SIZE = 262144;  // 512*32*16 以上，足够最大测试用例

    // 分配内存 (使用 static 确保对齐)
    static act_t in1[MAX_SIZE];
    static act_t in2[MAX_SIZE];
    static act_t out_hw[MAX_SIZE];
    static act_t out_ref[MAX_SIZE];

    bool all_pass = true;

    // 测试用例 1: 正常加法 (无溢出)
    cout << "\n=== Test Case 1: Normal Addition ===" << endl;
    int CH = 64, H = 56, W = 56;
    int size = CH * H * W;
    for (int i = 0; i < size; i++) {
        in1[i] = (act_t)(rand() % 50 - 25);  // [-25, 24]
        in2[i] = (act_t)(rand() % 50 - 25);
    }
    memset(out_hw, 0, size * sizeof(act_t));

    add_ip(in1, in2, out_hw, CH, H, W);
    ref_add(in1, in2, out_ref, size);
    all_pass &= check_result(out_hw, out_ref, size, "Normal Addition");

    // 测试用例 2: 正向溢出 (+)
    cout << "\n=== Test Case 2: Positive Overflow ===" << endl;
    for (int i = 0; i < size; i++) {
        in1[i] = 100;
        in2[i] = 50;
    }
    memset(out_hw, 0, size * sizeof(act_t));

    add_ip(in1, in2, out_hw, CH, H, W);
    ref_add(in1, in2, out_ref, size);
    all_pass &= check_result(out_hw, out_ref, size, "Positive Overflow");

    // 测试用例 3: 负向溢出 (-)
    cout << "\n=== Test Case 3: Negative Overflow ===" << endl;
    for (int i = 0; i < size; i++) {
        in1[i] = -100;
        in2[i] = -50;
    }
    memset(out_hw, 0, size * sizeof(act_t));

    add_ip(in1, in2, out_hw, CH, H, W);
    ref_add(in1, in2, out_ref, size);
    all_pass &= check_result(out_hw, out_ref, size, "Negative Overflow");

    // 测试用例 4: 非整除tile尺寸
    cout << "\n=== Test Case 4: Non-divisible Tile Size ===" << endl;
    CH = 13, H = 17, W = 19;
    size = CH * H * W;
    for (int i = 0; i < size; i++) {
        in1[i] = (act_t)(rand() % 100 - 50);
        in2[i] = (act_t)(rand() % 100 - 50);
    }
    memset(out_hw, 0, size * sizeof(act_t));

    add_ip(in1, in2, out_hw, CH, H, W);
    ref_add(in1, in2, out_ref, size);
    all_pass &= check_result(out_hw, out_ref, size, "Non-divisible Tile Size");

    // 测试用例 5: 单通道
    cout << "\n=== Test Case 5: Single Channel ===" << endl;
    CH = 1, H = 10, W = 10;
    size = CH * H * W;
    for (int i = 0; i < size; i++) {
        in1[i] = (act_t)(i - 50);
        in2[i] = (act_t)(50 - i);
    }
    memset(out_hw, 0, size * sizeof(act_t));

    add_ip(in1, in2, out_hw, CH, H, W);
    ref_add(in1, in2, out_ref, size);
    all_pass &= check_result(out_hw, out_ref, size, "Single Channel");

    // 测试用例 6: 混合边界情况
    cout << "\n=== Test Case 6: Edge Cases ===" << endl;
    CH = 4, H = 4, W = 4;
    size = CH * H * W;
    act_t test_vals[] = {-128, -127, -1, 0, 1, 126, 127};
    int idx = 0;
    for (int i = 0; i < size; i++) {
        in1[i] = test_vals[idx % 7];
        in2[i] = test_vals[(idx + 3) % 7];
        idx++;
    }
    memset(out_hw, 0, size * sizeof(act_t));

    add_ip(in1, in2, out_hw, CH, H, W);
    ref_add(in1, in2, out_ref, size);
    all_pass &= check_result(out_hw, out_ref, size, "Edge Cases");

    cout << "\n========================================" << endl;
    if (all_pass) {
        cout << "All tests PASSED!" << endl;
        return 0;
    } else {
        cout << "Some tests FAILED!" << endl;
        return 1;
    }
}
