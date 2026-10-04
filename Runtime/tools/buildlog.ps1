# Builds a game package without stopping at the first failure, saves the log next to its
# build dir and prints a summary.   buildlog.ps1 -GameDir <pkg>\Native [-Guest native|wasm] [-Target x]
param([Parameter(Mandatory = $true)][string]$GameDir, [ValidateSet('native', 'wasm')][string]$Guest = 'native',
      [string]$Target = '', [int]$Show = 25, [string]$Pattern = ': error:|undefined symbol|error:')
$GameDir = (Resolve-Path $GameDir).Path
$log = Join-Path $GameDir "build\last_build_$Guest.log"
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
$out = [System.Collections.Generic.List[string]]::new()
$start = Get-Date
try {
    $buildArgs = @{ GameDir = $GameDir; Guest = $Guest; KeepGoing = $true }
    if ($Target) { $buildArgs.Target = $Target }
    & (Join-Path $PSScriptRoot 'build_game.ps1') @buildArgs 2>&1 | ForEach-Object { $out.Add("$_") }
} catch { $out.Add("THROW: $_") }
$out | Set-Content -Path $log -Encoding utf8
"failed steps: " + @($out | Select-String -Pattern '^FAILED').Count
"errors: " + @($out | Select-String -Pattern ': error:').Count
"undefined symbols: " + @($out | Select-String -Pattern 'undefined symbol').Count
$out | Select-String -Pattern $Pattern | Select-Object -First $Show | ForEach-Object {
    $line = $_.Line -replace '^.*?[\/](src|port|host)[\/]', '$1/'
    $line.Substring(0, [Math]::Min(240, $line.Length))
}
"elapsed: {0:n0}s" -f ((Get-Date) - $start).TotalSeconds
