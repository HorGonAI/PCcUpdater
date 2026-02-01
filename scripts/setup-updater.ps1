Param(
    [Parameter(Mandatory = $true)]
    [string]$TargetDir,

    [string]$UpdaterExe = "",
    [string]$UpdaterName = "PCcUpdater"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $TargetDir)) {
    throw "Target directory does not exist: $TargetDir"
}

if ([string]::IsNullOrWhiteSpace($UpdaterExe)) {
    $UpdaterExe = Join-Path $PSScriptRoot "..\build\pc_updater.exe"
}

$UpdaterExe = (Resolve-Path $UpdaterExe).Path

[Environment]::SetEnvironmentVariable("PC_UPDATER_TARGET_DIR", $TargetDir, "User")

function Ensure-StartupShortcut {
    param(
        [string]$ExePath,
        [string]$Name
    )

    $startupPath = [Environment]::GetFolderPath("Startup")
    $shortcutPath = Join-Path $startupPath ("$Name.lnk")
    if (-not (Test-Path $shortcutPath)) {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($shortcutPath)
        $shortcut.TargetPath = $ExePath
        $shortcut.WorkingDirectory = Split-Path $ExePath
        $shortcut.Save()
    }
}

function Ensure-RunRegistry {
    param(
        [string]$ExePath,
        [string]$Name
    )

    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
    New-ItemProperty -Path $key -Name $Name -Value $ExePath -PropertyType String -Force | Out-Null
}

function Ensure-TaskScheduler {
    param(
        [string]$ExePath,
        [string]$Name
    )

    $taskName = "$Name Logon"
    $existing = schtasks /Query /TN $taskName 2>$null
    if (-not $?) {
        schtasks /Create /F /SC ONLOGON /RL HIGHEST /TN $taskName /TR $ExePath | Out-Null
    }
}

Ensure-StartupShortcut -ExePath $UpdaterExe -Name $UpdaterName
Ensure-RunRegistry -ExePath $UpdaterExe -Name $UpdaterName
Ensure-TaskScheduler -ExePath $UpdaterExe -Name $UpdaterName

Write-Host "Updater configured."
