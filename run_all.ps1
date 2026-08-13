# ================================================================
# run_all.ps1
# 一键运行所有HLS综合: conv_ip + pool_ip
# 用法: 右键以PowerShell运行, 或 .\run_all.ps1
# ================================================================

$VITIS_HLS = "E:\Xilinx\Vitis_HLS\2024.2\bin\vitis_hls.bat"
$WORK_DIR  = "E:\codes\microzed\fastvit_hls"

if (-not (Test-Path $VITIS_HLS)) {
    Write-Error "找不到 vitis_hls: $VITIS_HLS"
    exit 1
}

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  FastVIT HLS IP 综合流程" -ForegroundColor Cyan
Write-Host "  Vitis HLS 2024.2 / xc7z020clg400-1" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan

# --- conv_ip ---
Write-Host "`n[1/2] 综合 conv_ip ..." -ForegroundColor Yellow
Set-Location "$WORK_DIR\conv_ip"
# -nolog: 避免Windows缺少tee.exe导致的错误 (Vitis HLS 2024.2 bug on Windows)
cmd /c "cd /d $WORK_DIR\conv_ip && $VITIS_HLS -f run_hls.tcl -nolog" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "conv_ip 综合失败! 查看工程目录下的 vitis_hls.log"
    exit 1
}
Write-Host "[OK] conv_ip 综合完成" -ForegroundColor Green

# --- pool_ip ---
Write-Host "`n[2/2] 综合 pool_ip ..." -ForegroundColor Yellow
Set-Location "$WORK_DIR\pool_ip"
cmd /c "cd /d $WORK_DIR\pool_ip && $VITIS_HLS -f run_hls.tcl -nolog" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Error "pool_ip 综合失败! 查看 vitis_hls_pool.log"
    exit 1
}
Write-Host "[OK] pool_ip 综合完成" -ForegroundColor Green

Write-Host "`n============================================" -ForegroundColor Cyan
Write-Host "  ALL DONE" -ForegroundColor Green
Write-Host "  IP核输出位置:" -ForegroundColor Cyan
Write-Host "    conv: $WORK_DIR\conv_ip\conv_ip_proj\solution1\impl\ip\" -ForegroundColor White
Write-Host "    pool: $WORK_DIR\pool_ip\pool_ip_proj\solution1\impl\ip\" -ForegroundColor White
Write-Host "    gap:  $WORK_DIR\pool_ip\gap_ip_proj\solution1\impl\ip\" -ForegroundColor White
Write-Host "============================================" -ForegroundColor Cyan

Set-Location $WORK_DIR
