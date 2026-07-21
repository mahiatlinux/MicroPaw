param(
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"

function Invoke-ReportCommand {
    param(
        [string]$Program,
        [string[]]$Arguments,
        [string]$Path
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Program @Arguments 2>&1 | Tee-Object -FilePath $Path
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) {
        throw "$Program failed with exit code $code."
    }
}

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "Export an ESP-IDF environment before running this script."
}

New-Item -ItemType Directory -Force -Path reports | Out-Null
Invoke-ReportCommand idf.py @("build") reports/build.log
Invoke-ReportCommand idf.py @("size") reports/size.txt
Invoke-ReportCommand idf.py @("size-components") reports/components.txt

if ($Port) {
    Invoke-ReportCommand python @("scripts/runtime_report.py", $Port) reports/runtime.txt
}
