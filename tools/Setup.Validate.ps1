param(
    [string]$GameDir
)

$ErrorActionPreference = "Continue"

Write-Host ""
Write-Host "========================================"
Write-Host "Envy-Diamond-2 Post-Install Validation"
Write-Host "========================================"
Write-Host ""

if (-not $GameDir) {
    $GameDir = $PWD.Path
}

Write-Host "Checking installation in: $GameDir"
Write-Host ""

$checksPassed = 0
$checksFailed = 0
$checksWarning = 0

function Check-Result {
    param([string]$Name, [bool]$Passed, [string]$Message)
    
    if ($Passed) {
        Write-Host "  [OK] $Name" -ForegroundColor Green
        if ($Message) { Write-Host "    $Message" -ForegroundColor DarkGreen }
        $script:checksPassed++
    } else {
        if ($Message) {
            Write-Host "  [FAIL] $Name" -ForegroundColor Red
            Write-Host "         Message: $Message" -ForegroundColor Red
            $script:checksFailed++
        } else {
            Write-Host "  [WARN] $Name" -ForegroundColor Yellow
            $script:checksWarning++
        }
    }
}

Write-Host "1. Plugin Files" -ForegroundColor Cyan
Check-Result -Name "EnvyDynamicPacing.asi" -Passed (Test-Path "$GameDir\OptiScaler\plugins\EnvyDynamicPacing.asi")
Check-Result -Name "OptiScaler.dll" -Passed (Test-Path "$GameDir\OptiScaler.dll")
Check-Result -Name "OptiScaler.ini" -Passed (Test-Path "$GameDir\OptiScaler.ini")
Write-Host ""

Write-Host "2. TDR Registry Settings" -ForegroundColor Cyan
$tdrPath = "HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers"
$tdrKeys = @("TdrDelay", "TdrDdiDelay", "TdrLimitCount", "TdrLimitTime")

foreach ($key in $tdrKeys) {
    try {
        $value = Get-ItemProperty -Path $tdrPath -Name $key -ErrorAction Stop | Select-Object -ExpandProperty $key
        Check-Result -Name $key -Passed ($value -ne $null) -Message "Value: $value"
    } catch {
        Check-Result -Name $key -Passed $false -Message "Not configured (default: may cause timeouts)"
    }
}
Write-Host ""

Write-Host "3. OptiScaler.ini Configuration" -ForegroundColor Cyan
$iniContent = Get-Content "$GameDir\OptiScaler.ini" -ErrorAction SilentlyContinue

if ($iniContent) {
    $loadAsiPlugins = $iniContent | Where-Object { $_ -match "^LoadAsiPlugins=" }
    Check-Result -Name "LoadAsiPlugins=true" -Passed ($loadAsiPlugins -match "true")
} else {
    Check-Result -Name "OptiScaler.ini" -Passed $false -Message "Not found"
}
Write-Host ""

Write-Host "4. Environment Configuration" -ForegroundColor Cyan
$envFile = Get-Content "$GameDir\optiscaler.env" -ErrorAction SilentlyContinue
if ($envFile) {
    $gpuGen = $envFile | Where-Object { $_ -match "ENY_GPU_GEN=" }
    Check-Result -Name "GPU Gen File" -Passed ($gpuGen -ne $null) -Message $gpuGen
} else {
    Check-Result -Name "GPU Gen File" -Passed $false -Message "Not found"
}
Write-Host ""

Write-Host "5. Backup Files" -ForegroundColor Cyan
$backupFolders = Get-ChildItem $GameDir -Directory | Where-Object { $_.Name -match "backup-amd" }
if ($backupFolders) {
    $latestBackup = $backupFolders | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    Check-Result -Name "Backup folder" -Passed $true -Message $latestBackup.Name
    $tdrBackup = Test-Path "$($latestBackup.FullName)\tdr_backup.json"
    Check-Result -Name "TDR backup" -Passed $tdrBackup
} else {
    Check-Result -Name "Backup folder" -Passed $false -Message "Not found"
}
Write-Host ""

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Summary" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Passed:   $checksPassed" -ForegroundColor Green
Write-Host "Warnings: $checksWarning" -ForegroundColor Yellow
Write-Host "Failed:   $checksFailed" -ForegroundColor Red
Write-Host ""

if ($checksFailed -eq 0) {
    Write-Host "SUCCESS: Installation validated successfully!" -ForegroundColor Green
    if ($checksWarning -gt 0) {
        Write-Host "Note: $checksWarning warnings found. Review above." -ForegroundColor Yellow
    }
} else {
    Write-Host "FAILURE: Installation has issues. Review warnings above." -ForegroundColor Red
}
Write-Host ""
