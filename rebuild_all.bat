@echo off
REM ================================================================
REM rebuild_all.bat — 重新综合所有改动的 HLS IP 并更新 Vivado
REM
REM 改动:
REM   pwconv_ip v5: Input-Stationary + 64-bit AXI + BRAM cache
REM   dwconv_ip:    添加 DSP binding (修复 200MHz 时序)
REM
REM 前提: vitis_hls 和 vivado 在 PATH 中
REM ================================================================

echo === Step 1: pwconv_ip v5 HLS Synthesis ===
cd /d "%~dp0pwconv_ip"
vitis_hls -f run_hls_v5.tcl
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: pwconv_ip synthesis
    exit /b 1
)

echo === Step 2: dwconv_ip HLS Synthesis (200MHz) ===
cd /d "%~dp0dwconv_ip"
vitis_hls -f run_hls_200mhz.tcl
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: dwconv_ip synthesis
    exit /b 1
)

echo === Step 3: Vivado Implementation Update ===
cd /d "%~dp0vivado_impl"
vivado -mode batch -source update_impl.tcl -nolog -nojournal
if %ERRORLEVEL% NEQ 0 (
    echo FAILED: Vivado implementation
    exit /b 1
)

echo.
echo === ALL DONE ===
echo Bitstream: vivado_impl\fastvit_util_check\fastvit_util_check.runs\impl_1\fastvit_bd_wrapper.bit
