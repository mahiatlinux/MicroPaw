$ErrorActionPreference = "Stop"
$baseUrl = "https://github.com/mahiatlinux/MicroPaw/releases/latest/download"
$pythonCommand = @("py", "python3.14", "python3.13", "python3.12", "python3.11", "python3.10", "python3", "python") |
    Where-Object { Get-Command $_ -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $pythonCommand) {
    throw "Python 3.10 or newer is required."
}
$pythonPrefix = if ($pythonCommand -eq "py") { @("-3") } else { @() }

function Invoke-Python([string[]]$Arguments) {
    $pythonArguments = $pythonPrefix + $Arguments
    & $pythonCommand @pythonArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Python command failed."
    }
}

Invoke-Python @("-c", "import sys; raise SystemExit(sys.version_info < (3, 10))")
$tempDir = Join-Path ([IO.Path]::GetTempPath()) ("micropaw-" + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($tempDir) | Out-Null
try {
    $image = Join-Path $tempDir "micropaw-usb.hex"
    $checksums = Join-Path $tempDir "SHA256SUMS"
    Invoke-WebRequest "$baseUrl/micropaw-usb.hex" -OutFile $image
    Invoke-WebRequest "$baseUrl/SHA256SUMS" -OutFile $checksums
    $line = Get-Content $checksums | Where-Object { $_ -match "\s+micropaw-usb\.hex$" } | Select-Object -First 1
    if (-not $line) {
        throw "Firmware checksum is missing."
    }
    $expected = ($line -split "\s+")[0].ToLowerInvariant()
    $actual = (Get-FileHash $image -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "Firmware checksum verification failed."
    }
    $venv = Join-Path $tempDir "venv"
    Invoke-Python @("-m", "venv", $venv)
    $pythonCommand = Join-Path $venv "Scripts\python.exe"
    $pythonPrefix = @()
    Invoke-Python @("-m", "pip", "install", "--upgrade", "esptool~=5.0")
    Write-Host "Connecting to the ESP32-S3. Hold BOOT now if automatic reset is unavailable."
    $flashArguments = @("-m", "esptool", "--chip", "esp32s3")
    if ($env:MICROPAW_PORT) {
        $flashArguments += @("--port", $env:MICROPAW_PORT)
    }
    $flashArguments += @("--before", "default-reset", "--after", "hard-reset", "write-flash", "0x0", $image)
    Invoke-Python $flashArguments
    Write-Host "MicroPaw flash complete."
}
finally {
    Remove-Item $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
