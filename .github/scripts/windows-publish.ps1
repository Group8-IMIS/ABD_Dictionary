[CmdletBinding()]
param (
    [string] $archiveName, [string] $targetName
)
# 外部环境变量包括:
# archiveName: ${{ matrix.qt_ver }}-${{ matrix.qt_arch }}
# winSdkDir: ${{ steps.build.outputs.winSdkDir }}
# winSdkVer: ${{ steps.build.outputs.winSdkVer }}
# vcToolsInstallDir: ${{ steps.build.outputs.vcToolsInstallDir }}
# vcToolsRedistDir: ${{ steps.build.outputs.vcToolsRedistDir }}
# msvcArch: ${{ matrix.msvc_arch }}


# winSdkDir: C:\Program Files (x86)\Windows Kits\10\ 
# winSdkVer: 10.0.19041.0\ 
# vcToolsInstallDir: C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Tools\MSVC\14.28.29333\ 
# vcToolsRedistDir: C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Redist\MSVC\14.28.29325\ 
# archiveName: 5.9.9-win32_msvc2015
# msvcArch: x86

$scriptDir = $PSScriptRoot
$currentDir = Get-Location
Write-Host "currentDir" $currentDir
Write-Host "scriptDir" $scriptDir

function Main() {

    New-Item -ItemType Directory $archiveName
    New-Item -ItemType Directory $archiveName\locale
    New-Item -ItemType Directory $archiveName\opencc
    # 拷贝exe
    Copy-Item release\$targetName $archiveName\
    Write-Host "copy item finished..."

    # #拷贝pdb
    # Copy-Item release\*.pdb $archiveName\
    # Write-Host "copy pdb finished..."
    # 拷贝依赖
    windeployqt --qmldir . --plugindir $archiveName\plugins --compiler-runtime $archiveName\$targetName
    # 删除不必要的文件
#    $excludeList = @("*.qmlc", "*.ilk", "*.exp", "*.lib", "*.pdb")
    $excludeList = @("*.qmlc", "*.ilk", "*.exp", "*.lib")
    Remove-Item -Path $archiveName -Include $excludeList -Recurse -Force
    Write-Host "remove item finished..."
    # 拷贝vcRedist dll
    $redistDll="{0}{1}\*.CRT\*.dll" -f $env:vcToolsRedistDir.Trim(),$env:msvcArch
    Write-Host "redist dll $($redistDll)"
    Copy-Item $redistDll $archiveName\
    Write-Host "copy redist dll..."
    Copy-Item "LICENSE.txt" $archiveName\
    Write-Host "copy license.."
    Copy-Item "opencc\*" $archiveName\opencc\
    Write-Host "opencc config files.."

    # 拷贝WinSDK dll
    $sdkDll="{0}Redist\{1}ucrt\DLLs\{2}\*.dll" -f $env:winSdkDir.Trim(),$env:winSdkVer.Trim(),$env:msvcArch
    Write-Host "copy sdk dll$($sdkDll)"
    Copy-Item $sdkDll $archiveName\
    Copy-Item winlibs\lib\msvc\*.dll $archiveName\
    Copy-Item winlibs\lib\*.dll $archiveName\
    Copy-Item locale\*.qm $archiveName\locale\

    $webengineqm="{0}\translations\qtwebengine_*.qm" -f $env:QTDIR.Trim()
    Write-Host "copy qtwebengine qm from $($webengineqm)"
    Copy-Item $webengineqm $archiveName\locale\

    # $multimedia="{0}\plugins\multimedia" -f $env:QTDIR.Trim()
    # if(Test-Path $multimedia){
    # Write-Host "copy multimedia  $($multimedia) to plugins"
    # Copy-Item -Path $multimedia -Destination $archiveName\plugins -Recurse
    # }

    Write-Host "compress zip..."
    # 打包zip
    Compress-Archive -Path $archiveName -DestinationPath $archiveName'.zip'
}

if ($null -eq $archiveName || $null -eq $targetName) {
    Write-Host "args missing, archiveName is" $archiveName ", targetName is" $targetName
    return
}
Main


