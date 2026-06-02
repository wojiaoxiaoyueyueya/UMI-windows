# collect_dlls.ps1 - Collect all runtime DLL dependencies for portability
# Usage: powershell -ExecutionPolicy Bypass -File collect_dlls.ps1 -BuildDir ./build

param(
    [string]$BuildDir = ".",
    [string]$MingwBin = "C:\msys64\mingw64\bin"
)

$ErrorActionPreference = "Continue"
$buildDir = (Resolve-Path $BuildDir).Path

$systemDlls = @(
    'KERNEL32', 'msvcrt', 'WS2_32', 'USER32', 'GDI32', 'ADVAPI32', 'SHELL32',
    'OLE32', 'OLEAUT32', 'dbghelp', 'WINMM', 'CRYPT32', 'Secur32', 'bcrypt',
    'ntdll', 'SHLWAPI', 'SETUPAPI', 'AVICAP32', 'USERENV', 'AVRT', 'ncrypt',
    'gdiplus', 'DWrite', 'USP10', 'OPENGL32', 'd3d11', 'dxgi', 'dwmapi',
    'uxtheme', 'msimg32', 'imm32', 'version', 'wintrust', 'oleacc', 'msasn1',
    'netapi32', 'iphlpapi', 'dnsapi', 'winhttp', 'wldap32', 'mpr', 'netutils',
    'samlib', 'wtsapi32', 'propsys', 'shcore', 'cfgmgr32', 'powrprof',
    'kernelbase', 'cryptbase', 'sspicli', 'rpcrt4', 'combase', 'sechost',
    'profapi', 'mswsock', 'ws2help', 'wsock32', 'api-ms-'
)

function Test-SystemDll($name) {
    foreach ($sys in $systemDlls) {
        if ($name -like "$sys*") { return $true }
    }
    return $false
}

$exe = Get-ChildItem -Path "$buildDir\*.exe" | Select-Object -First 1
if (-not $exe) {
    Write-Error "No exe found in $buildDir"
    exit 1
}

$objdump = Join-Path $MingwBin "objdump.exe"
if (-not (Test-Path $objdump)) {
    Write-Error "objdump not found at $objdump"
    exit 1
}

Write-Output "=== DLL Dependency Collector ==="
Write-Output "Build dir: $buildDir"
Write-Output "MinGW bin: $MingwBin"
Write-Output "Exe: $($exe.Name)"
Write-Output ""

$queue = @($exe.Name)
$checked = @{}
$copied = 0

while ($queue.Count -gt 0) {
    $current = $queue[0]
    $queue = if ($queue.Count -gt 1) { $queue[1..($queue.Count-1)] } else { @() }
    if ($checked.ContainsKey($current)) { continue }
    $checked[$current] = $true

    $fullPath = Join-Path $buildDir $current
    if (-not (Test-Path $fullPath)) { continue }

    $output = & $objdump -p $fullPath 2>$null
    foreach ($line in $output) {
        if ($line -match 'DLL Name:\s+(\S+)') {
            $dep = $Matches[1]
            if (Test-SystemDll $dep) { continue }
            if ($checked.ContainsKey($dep)) { continue }

            $depPath = Join-Path $buildDir $dep
            if (-not (Test-Path $depPath)) {
                $srcPath = Join-Path $MingwBin $dep
                if (Test-Path $srcPath) {
                    Copy-Item $srcPath $buildDir -Force
                    Write-Output "  + $dep"
                    $copied++
                    $queue += $dep
                }
            } else {
                $queue += $dep
            }
        }
    }
}

Write-Output ""
Write-Output "=== Done! Copied $copied DLLs ==="
Write-Output "You can now copy the entire build directory to any Windows PC."
