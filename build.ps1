$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'git was not found in PATH.'
}
if (-not (Get-Command xmake -ErrorAction SilentlyContinue)) {
    throw 'xmake was not found in PATH. Install XMake 3.0.0 or newer.'
}

if (-not (Test-Path '.\lib\commonlibsf\xmake.lua')) {
    Write-Host 'Cloning current libxse/CommonLibSF and submodules...'
    git clone --recurse-submodules https://github.com/libxse/commonlibsf.git .\lib\commonlibsf
    if ($LASTEXITCODE -ne 0) { throw 'CommonLibSF clone failed.' }
} else {
    Write-Host 'Using existing .\lib\commonlibsf'
}

Write-Host 'Configuring releasedbg x64 build...'
xmake f -m releasedbg -a x64 -y
if ($LASTEXITCODE -ne 0) { throw 'xmake configure failed.' }

Write-Host 'Building RobinStackKiller...'
xmake build RobinStackKiller -y
if ($LASTEXITCODE -ne 0) { throw 'xmake build failed.' }

Write-Host ''
Write-Host 'Build complete. Look under .\build\windows\x64\releasedbg\ for RobinStackKiller.dll.'
