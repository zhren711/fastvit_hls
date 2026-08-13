@echo off
cd /d V:\vivado_impl
set PATH=E:\Xilinx\Vivado\2024.2\bin;%PATH%
call vivado.bat -mode batch -source resume_round5_from_physopt.tcl -nolog -nojournal > V:\vivado_impl\resume_round5_stdout.log 2> V:\vivado_impl\resume_round5_stderr.log
echo DONE_EXIT_%ERRORLEVEL% >> V:\vivado_impl\resume_round5_stdout.log
