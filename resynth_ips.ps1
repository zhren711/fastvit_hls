# resynth_ips.ps1
# 重新综合并打包 conv_ip / pwconv_ip / pool_ip
# 原因:
#   conv_ip  - impl/ip 是旧版 v1 (含DW/PW), 需从 v2 源码重新 export
#   pwconv_ip - tiling 已调小 (PW_TN/TM 4→2, PW_TS 16→8)
#   pool_ip  - tiling 已调小 (POOL_TN 2→1, POOL_TR/TC 8→4)
#
# 用法: powershell -ExecutionPolicy Bypass -File resynth_ips.ps1

$env:PATH = "E:\Xilinx\Vitis_HLS\2024.2\bin;" + $env:PATH
$root = "E:\codes\microzed\fastvit_hls"

$ips = @(
    @{ name="conv_ip";   dir="$root\conv_ip"   },
    @{ name="pwconv_ip"; dir="$root\pwconv_ip" },
    @{ name="pool_ip";   dir="$root\pool_ip"   }
)

foreach ($ip in $ips) {
    Write-Host ""
    Write-Host "============================================" -ForegroundColor Cyan
    Write-Host "  Synthesizing $($ip.name)..." -ForegroundColor Cyan
    Write-Host "============================================" -ForegroundColor Cyan

    Push-Location $ip.dir
    $log = "$($ip.dir)\hls_$($ip.name)_resynth.log"

    # 强制清理旧 impl 避免缓存残留
    $implDir = "$($ip.dir)\$($ip.name)_proj\solution1\impl"
    if (Test-Path $implDir) {
        Write-Host "  Removing old impl/..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $implDir
    }

    vitis_hls -f run_hls.tcl 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: $($ip.name) synthesis FAILED. Check $log" -ForegroundColor Red
    } else {
        Write-Host "OK: $($ip.name) done -> impl/ip ready" -ForegroundColor Green
    }
    Pop-Location
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "  All IPs re-synthesized."                  -ForegroundColor Green
Write-Host "  Next: run Vivado implementation"          -ForegroundColor Green
Write-Host "    cd E:\codes\microzed\fastvit_hls\vivado_impl" -ForegroundColor White
Write-Host "    vivado -mode batch -source run_impl.tcl -nolog -nojournal" -ForegroundColor White
Write-Host "============================================" -ForegroundColor Green
