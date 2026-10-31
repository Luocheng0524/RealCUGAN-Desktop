param(
    [Parameter(Mandatory=$true)][string]$ToolsRoot,
    [string]$PackageRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$BuildRoot='',
    [string]$EvidenceRoot='',
    [ValidateRange(1,16)][int]$Parallel=2
)
# 主程序维护入口；版本生成、CMake 参数、指纹与发布流程统一由 build.ps1 实现。
$ErrorActionPreference='Stop'
$options=@{ToolsRoot=$ToolsRoot;PackageRoot=$PackageRoot;Parallel=$Parallel;MainOnly=$true}
if($BuildRoot){$options.BuildRoot=$BuildRoot}
else{$options.BuildRoot=Join-Path $PackageRoot 'build-main'}
if($EvidenceRoot){$options.EvidenceRoot=$EvidenceRoot}
else{$options.EvidenceRoot=Join-Path $PackageRoot 'build-main-records'}
& (Join-Path $PSScriptRoot 'build.ps1') @options
