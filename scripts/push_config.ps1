param(
    [Parameter(Mandatory, Position = 0)]
    [string]$File,
    [string]$Port,
    [switch]$Reboot
)

$ErrorActionPreference = "Stop"

function Wait-For([IO.Ports.SerialPort]$Serial, [string]$Expected, [int]$Seconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    $buffer = ""
    while ([DateTime]::UtcNow -lt $deadline) {
        $buffer += $Serial.ReadExisting()
        while (($index = $buffer.IndexOf("`n")) -ge 0) {
            $line = $buffer.Substring(0, $index).Trim()
            $buffer = $buffer.Substring($index + 1)
            if ($line.StartsWith("CONFIG ERROR")) {
                throw $line
            }
            if ($line -eq $Expected) {
                return
            }
        }
        Start-Sleep -Milliseconds 20
    }
    throw "Timed out waiting for $Expected"
}

$data = [IO.File]::ReadAllBytes((Resolve-Path $File))
[Text.UTF8Encoding]::new($false, $true).GetString($data) | Out-Null
if ($data.Length -lt 1 -or $data.Length -gt 4096) {
    throw "Configuration must be 1-4096 bytes"
}
if (-not $Port) {
    $ports = @([IO.Ports.SerialPort]::GetPortNames())
    if ($ports.Count -ne 1) {
        $found = if ($ports.Count) { $ports -join ", " } else { "none" }
        throw "Expected one serial port, found $found. Use -Port PORT."
    }
    $Port = $ports[0]
}

$serial = [IO.Ports.SerialPort]::new($Port, 115200, [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One)
$serial.ReadTimeout = 100
$serial.WriteTimeout = 2000
$serial.DtrEnable = $false
$serial.RtsEnable = $false
try {
    $serial.Open()
    Start-Sleep -Seconds 1
    $serial.DiscardInBuffer()
    $serial.Write("push-config $($data.Length)`n")
    Wait-For $serial "CONFIG READY" 5
    for ($offset = 0; $offset -lt $data.Length; $offset += 64) {
        $length = [Math]::Min(64, $data.Length - $offset)
        $serial.Write($data, $offset, $length)
        Start-Sleep -Milliseconds 30
    }
    Wait-For $serial "CONFIG OK" 10
    if ($Reboot) {
        $serial.Write("reboot`n")
    }
}
finally {
    $serial.Dispose()
}

Write-Host ("Configuration saved." + $(if ($Reboot) { " Reboot requested." } else { " Reboot to apply network changes." }))
