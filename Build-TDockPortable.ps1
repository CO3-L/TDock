[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist'))

$ErrorActionPreference = 'Stop'
$dist = Join-Path $PSScriptRoot 'dist'
$app = Join-Path $dist 'TDock.exe'
$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($app)
$version = '{0}.{1}.{2}' -f $versionInfo.FileMajorPart, $versionInfo.FileMinorPart, $versionInfo.FileBuildPart
$bundle = "TDock-$version-win10-x64"
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$source = Join-Path $PSScriptRoot 'src'
$icon = Join-Path $PSScriptRoot 'assets\TDock.ico'
$appManifest = Join-Path $source 'TDock.manifest'
$expectedVersion = ([xml](Get-Content -LiteralPath $appManifest -Raw)).assembly.assemblyIdentity.version
if ("$version.$($versionInfo.FilePrivatePart)" -ne $expectedVersion) {
    throw 'TDock.exe does not match the source version. Run Build-TDock.ps1 before packaging.'
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stage = Join-Path ([IO.Path]::GetTempPath()) ('TDock-package-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    $packageNames = @('README.md', 'MinHook-LICENSE.txt', 'TDockNative.dll', 'TDock.exe')
    foreach ($name in $packageNames) {
        $inputFile = if ($name -eq 'README.md') {
            Join-Path $PSScriptRoot 'README-portable.md'
        } else {
            Join-Path $dist $name
        }
        $outputFile = Join-Path $stage $name
        Copy-Item -LiteralPath $inputFile -Destination $outputFile
        if ($name -eq 'README.md') {
            $license = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'LICENSE')).TrimEnd()
            [IO.File]::AppendAllText($outputFile, "`r`n`r`n## TDock license`r`n`r`n" + $license + "`r`n")
        }
    }
    $packageFiles = $packageNames | ForEach-Object { Join-Path $stage $_ }
    $fileList = Join-Path $stage 'PackageFiles.txt'
    $digests = foreach ($file in $packageFiles) { (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
    @($bundle) + $digests | Set-Content -LiteralPath $fileList -Encoding Ascii

    $versionSource = Join-Path $stage 'PortableVersion.cs'
    @(
        "[assembly: System.Reflection.AssemblyVersion(""$version.0"")]"
        "[assembly: System.Reflection.AssemblyFileVersion(""$version.0"")]"
    ) | Set-Content -LiteralPath $versionSource -Encoding Ascii
    $uninstaller = Join-Path $stage 'TDock-Uninstall.exe'
    $sources = @('PortableUninstall.cs', 'PortableCleanup.cs', 'PendingCacheCleanup.cs', 'ExplorerRestart.cs',
        'ControllerWindow.cs', 'StartupShortcut.cs') |
        ForEach-Object { Join-Path $source $_ }
    $options = @(
        '/nologo', '/codepage:65001', '/target:winexe', '/platform:x64', '/optimize+', "/out:$uninstaller",
        "/win32manifest:$appManifest", "/win32icon:$icon", "/resource:$fileList,TDock.PortableFiles,private",
        '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:Microsoft.CSharp.dll'
    )
    & $compiler @options @sources $versionSource
    if ($LASTEXITCODE -ne 0) { throw 'Portable uninstaller compilation failed.' }

    $zip = Join-Path $stage "$bundle.zip"
    Compress-Archive -LiteralPath ($packageFiles + $uninstaller) -DestinationPath $zip -CompressionLevel Optimal
    $checksum = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    "$checksum  $bundle.zip" | Set-Content -LiteralPath "$zip.sha256" -Encoding Ascii

    Copy-Item -LiteralPath $zip, "$zip.sha256" -Destination $OutputDirectory -Force
    Get-Item -LiteralPath (Join-Path $OutputDirectory "$bundle.zip"), (Join-Path $OutputDirectory "$bundle.zip.sha256")
} finally {
    Get-ChildItem -LiteralPath $stage -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
    [IO.Directory]::Delete($stage)
}
