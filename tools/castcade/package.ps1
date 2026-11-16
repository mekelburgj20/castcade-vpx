<#
.SYNOPSIS
  Builds the fork with its commit stamped into the version string and assembles the tester packages.

.DESCRIPTION
  1. Refuses to run with uncommitted changes to tracked files (the stamp must name the code that was built).
  2. Stamps git_version.h with "castcade-<sha7>" (as CI stamps the upstream build), builds with -Build, restores
     git_version.h afterwards whatever happens.
  3. Writes, under -Out\castcade-vpx-<sha7>\ :
       dropin\   VPinballX_GL64_castcade.exe + the launchers: for a folder that already holds Panasony's 10.8.0 build
       full\     the whole build output (exe renamed, DLLs, shaders, scripts, assets) + the launchers
     and a zip of each beside them.
  The launchers are written with CRLF line endings (batch files need them).
#>
[CmdletBinding()]
param(
    [string]$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).ProviderPath,
    [string]$Build = 'C:\code\repos\castcade-vpx-deps\build.ps1',
    [string]$Out = 'C:\code\repos\castcade-vpx-deps\dist'
)
$ErrorActionPreference = 'Stop'

Push-Location $Repo
try {
    $dirty = @(git status --porcelain --untracked-files=no)
    if ($dirty.Count) { throw "Uncommitted changes in ${Repo}:`n$($dirty -join "`n")" }
    $sha = (git rev-parse --short=7 HEAD).Trim()
} finally { Pop-Location }

$versionFile = Join-Path $Repo 'git_version.h'
$original = [IO.File]::ReadAllBytes($versionFile)
try {
    $text = [Text.Encoding]::ASCII.GetString($original)
    if ($text -notmatch '"unknown"') { throw 'git_version.h does not contain "unknown": already stamped?' }
    [IO.File]::WriteAllText($versionFile, $text.Replace('"unknown"', "`"castcade-$sha`""), [Text.Encoding]::ASCII)
    & pwsh -NoProfile -File $Build -Repo $Repo
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
} finally {
    [IO.File]::WriteAllBytes($versionFile, $original)
}

$release = Join-Path $Repo 'build\Release'
$exe = Join-Path $release 'VPinballX_GL64.exe'
$root = Join-Path $Out "castcade-vpx-$sha"
if (Test-Path $root) { Remove-Item -Recurse -Force $root }
$dropin = Join-Path $root 'dropin'
$full = Join-Path $root 'full'
New-Item -ItemType Directory -Force $dropin, $full | Out-Null

function Copy-Launchers([string]$to) {
    foreach ($f in Get-ChildItem (Join-Path $PSScriptRoot 'launchers') -File) {
        $content = [IO.File]::ReadAllText($f.FullName) -replace "`r`n", "`n" -replace "`n", "`r`n"
        [IO.File]::WriteAllText((Join-Path $to $f.Name), $content, (New-Object Text.UTF8Encoding $false))
    }
}

Copy-Item $exe (Join-Path $dropin 'VPinballX_GL64_castcade.exe')
Copy-Launchers $dropin

# The build output is a complete runnable folder (what CI uploads): DLLs, assets, shaders, scripts.
# Left out: build by-products, and what test runs from this folder leave behind (logs, the user folder).
Copy-Item (Join-Path $release '*') $full -Recurse -Exclude 'VPinballX_GL64.exe', '*.pdb', '*.ilk', '*.exp', '*.lib', '*.log', 'user'
Copy-Item $exe (Join-Path $full 'VPinballX_GL64_castcade.exe')
Copy-Launchers $full

foreach ($name in 'dropin', 'full') {
    $zip = Join-Path $root "castcade-vpx-$sha-$name.zip"
    Compress-Archive -Path (Join-Path (Join-Path $root $name) '*') -DestinationPath $zip -Force
    '{0}  {1:N1} MB  sha256 {2}' -f $zip, ((Get-Item $zip).Length / 1MB), (Get-FileHash $zip -Algorithm SHA256).Hash
}
"exe version: $((Get-Item (Join-Path $dropin 'VPinballX_GL64_castcade.exe')).VersionInfo.FileVersion)"
"packages: $root"
