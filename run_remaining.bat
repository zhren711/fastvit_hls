@echo off
set PATH=E:\Xilinx\Vivado\2024.2\gnuwin\bin;E:\Xilinx\Vitis_HLS\2024.2\bin;%PATH%

echo [%TIME%] Starting dwconv_ip...
cd /d E:\codes\microzed\fastvit_hls\dwconv_ip
call vitis_hls -f run_hls.tcl > hls_dwconv.log 2>&1
echo [%TIME%] dwconv_ip done (exit %ERRORLEVEL%).

echo [%TIME%] Starting pwconv_ip...
cd /d E:\codes\microzed\fastvit_hls\pwconv_ip
call vitis_hls -f run_hls.tcl > hls_pwconv.log 2>&1
echo [%TIME%] pwconv_ip done (exit %ERRORLEVEL%).

echo [%TIME%] Starting add_ip...
cd /d E:\codes\microzed\fastvit_hls\add_ip
call vitis_hls -f run_hls.tcl > hls_add.log 2>&1
echo [%TIME%] add_ip done (exit %ERRORLEVEL%).

echo [%TIME%] === ALL DONE ===
