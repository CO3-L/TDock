[CmdletBinding()]
param([string]$CMakePath)

$ErrorActionPreference = 'Stop'
if (-not $CMakePath) {
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmake) {
        $CMakePath = $cmake.Source
    } else {
        $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
            $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($visualStudio) {
                $CMakePath = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            }
        }
    }
}
if (-not $CMakePath -or -not (Test-Path -LiteralPath $CMakePath -PathType Leaf)) {
    throw 'CMake was not found. Provide -CMakePath or install Visual Studio C++ tools with CMake and the Windows SDK.'
}

$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw 'The .NET Framework C# compiler was not found.'
}

$build = Join-Path $PSScriptRoot 'build'
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null

& $CMakePath -S $PSScriptRoot -B $build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Native project configuration failed.' }
& $CMakePath --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Native compilation failed.' }
& $CMakePath --install $build --config Release --prefix $dist
if ($LASTEXITCODE -ne 0) { throw 'Could not stage the native library.' }

$source = Join-Path $PSScriptRoot 'src'
$sources = @('TDock.cs','TrayPanel.cs','TrayPanelEdge.cs','TrayMenuTheme.cs','NativeEffects.cs','RuntimeLibrary.cs','Preferences.cs','StartupShortcut.cs','ControllerWindow.cs') | ForEach-Object { Join-Path $source $_ }
$manifest = Join-Path $source 'TDock.manifest'
$icon = Join-Path $PSScriptRoot 'assets\TDock.ico'
& $compiler /nologo /codepage:65001 /target:winexe /platform:x64 /optimize+ "/win32manifest:$manifest" "/win32icon:$icon" "/out:$dist\TDock.exe" /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:Microsoft.CSharp.dll @sources
if ($LASTEXITCODE -ne 0) { throw 'Tray application compilation failed.' }

Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party\minhook-1.3.4\LICENSE.txt') -Destination (Join-Path $dist 'MinHook-LICENSE.txt') -Force

Get-ChildItem -LiteralPath $dist -File
