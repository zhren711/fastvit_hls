$vhls = "E:\Xilinx\Vitis_HLS\2024.2\bin\vitis_hls.bat"

Write-Host "[$(Get-Date -Format 'HH:mm:ss')] Starting dwconv_ip..."
Set-Location E:\codes\microzed\fastvit_hls\dwconv_ip
& $vhls -f run_hls.tcl > hls_dwconv.log 2>&1
Write-Host "[$(Get-Date -Format 'HH:mm:ss')] dwconv_ip done (exit $LASTEXITCODE)."

Write-Host "[$(Get-Date -Format 'HH:mm:ss')] Starting pwconv_ip..."
Set-Location E:\codes\microzed\fastvit_hls\pwconv_ip
& $vhls -f run_hls.tcl > hls_pwconv.log 2>&1
Write-Host "[$(Get-Date -Format 'HH:mm:ss')] pwconv_ip done (exit $LASTEXITCODE)."

Write-Host "[$(Get-Date -Format 'HH:mm:ss')] Starting add_ip..."
Set-Location E:\codes\microzed\fastvit_hls\add_ip
& $vhls -f run_hls.tcl > hls_add.log 2>&1
Write-Host "[$(Get-Date -Format 'HH:mm:ss')] add_ip done (exit $LASTEXITCODE)."

Write-Host "[$(Get-Date -Format 'HH:mm:ss')] === ALL DONE ==="
