param(
    [string]$PortName = "COM56",
    [int]$Seconds = 30,
    [switch]$Reset
)
$port = New-Object System.IO.Ports.SerialPort($PortName, 115200, "None", 8, "One")
$port.ReadTimeout = 200
$port.DtrEnable = $false
$port.RtsEnable = $false
try { $port.Open() } catch { Write-Output "OPEN FAILED: $_"; exit 1 }

if ($Reset) {
    # CH343 auto-reset: RTS high (EN low) 120 ms, then release
    $port.RtsEnable = $true
    Start-Sleep -Milliseconds 120
    $port.RtsEnable = $false
    Write-Output "[reset toggled]"
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$sb = New-Object System.Text.StringBuilder
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try {
        $chunk = $port.ReadExisting()
        if ($chunk) { [void]$sb.Append($chunk) }
    } catch {}
    Start-Sleep -Milliseconds 100
}
$port.Close()
$out = $sb.ToString()
if ($out.Length -eq 0) { Write-Output "[no output in $Seconds s]" }
else { Write-Output $out }
