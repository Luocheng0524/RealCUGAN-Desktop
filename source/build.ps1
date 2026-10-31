param(
    [Parameter(Mandatory=$true)][string]$ToolsRoot,
    [string]$VulkanInclude='',
    [string]$VulkanLibrary='',
    [string]$PackageRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$BuildRoot=(Join-Path $PSScriptRoot '..\build'),
    [string]$EvidenceRoot=(Join-Path $PSScriptRoot '..\build-records'),
    [ValidateRange(1,16)][int]$Parallel=2,
    [switch]$MainOnly
)
# Compile/link only. This script NEVER runs the app, worker, CTest or tests.
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$packageRoot=[IO.Path]::GetFullPath($PackageRoot)
$sourceRoot=Join-Path $packageRoot 'source'
$ToolsRoot=[IO.Path]::GetFullPath($ToolsRoot)
$BuildRoot=[IO.Path]::GetFullPath($BuildRoot)
$EvidenceRoot=[IO.Path]::GetFullPath($EvidenceRoot)
New-Item -ItemType Directory -Path $BuildRoot,$EvidenceRoot -Force | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath=Join-Path $EvidenceRoot ("build-$stamp.log")
$recordPath=Join-Path $EvidenceRoot ("build-$stamp.json")
$utf8=[Text.UTF8Encoding]::new($false)
function Save-Json($Path,$Value){[IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 12),$utf8)}
function CMake-Path([string]$Path){return $Path.Replace('\','/')}
function Tool([string]$Relative){
    $path=Join-Path $ToolsRoot $Relative
    if(!(Test-Path -LiteralPath $path)){$path=Join-Path $ToolsRoot ('bin/'+[IO.Path]::GetFileName($Relative))}
    if(-not(Test-Path -LiteralPath $path)){throw "Required fixed tool missing: $path"}
    return (CMake-Path $path)
}
function Run-Native([string]$Exe,[string[]]$Arguments){
    "COMMAND: $Exe $($Arguments -join ' ')" | Tee-Object -FilePath $logPath -Append
    & $Exe @Arguments 2>&1 | ForEach-Object { $_.ToString() | Tee-Object -FilePath $logPath -Append }
    $code=$LASTEXITCODE
    "EXIT_CODE: $code" | Tee-Object -FilePath $logPath -Append
    if($code -ne 0){throw "Build command failed ($code): $Exe"}
}
function Input-Hashes {
    $items=[Collections.Generic.List[object]]::new()
    $roots=@(
        @{Base=$packageRoot;Path=$sourceRoot;Label='source'},
        @{Base=$packageRoot;Path=(Join-Path $packageRoot 'models');Label='models'}
    )
    foreach($relative in @('bin','lib','include','share/cmake','w64devkit','cmake\bin','cmake\share','ninja')){
        $roots+=@{Base=$ToolsRoot;Path=(Join-Path $ToolsRoot $relative);Label='tools'}
    }
    if(!$MainOnly){
        $roots+=@{Base=([IO.Path]::GetFullPath($VulkanInclude));Path=([IO.Path]::GetFullPath($VulkanInclude));Label='vulkan-headers'}
        $roots+=@{Base=(Split-Path -Parent ([IO.Path]::GetFullPath($VulkanLibrary)));Path=([IO.Path]::GetFullPath($VulkanLibrary));Label='vulkan-library'}
    }
    foreach($root in $roots){
        if(!(Test-Path -LiteralPath $root.Path)){continue}
        foreach($file in Get-ChildItem -LiteralPath $root.Path -Recurse -File -Force | Sort-Object FullName){
            if($file.FullName -match '[\\/](__pycache__|\.git)[\\/]' -or $file.Extension -eq '.pyc'){continue}
            $relative=[IO.Path]::GetRelativePath($root.Base,$file.FullName).Replace('\','/')
            $label=if($root.Label -eq 'tools'){'tools/'+$relative}elseif($root.Label -like 'vulkan-*'){$root.Label+'/'+$relative}else{$relative}
            $items.Add([pscustomobject]@{Path=$label;Bytes=$file.Length;SHA256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()})
        }
    }
    if($MainOnly){
        foreach($name in @('realcugan-worker.exe','realcugan-gpu-worker.exe')){
            $file=Join-Path $packageRoot $name
            $items.Add([pscustomobject]@{Path=$name;Bytes=(Get-Item -LiteralPath $file).Length;SHA256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()})
        }
    }
    return $items.ToArray()
}
$record=[ordered]@{
    status='building';started=(Get-Date -Format o);ended=$null
    source=(CMake-Path $sourceRoot);tools=(CMake-Path $ToolsRoot);build=(CMake-Path $BuildRoot);main_only=[bool]$MainOnly
    runtime_tests_executed=$false;parallel=$Parallel;powershell=$PSVersionTable.PSVersion.ToString()
    before=$null;after=$null;inputs_unchanged=$false;artifacts=@();error=$null
}
try{
    $cmake=Tool 'cmake\bin\cmake.exe';$ninja=Tool 'ninja\ninja.exe'
    $cc=Tool 'w64devkit\bin\gcc.exe';$cxx=Tool 'w64devkit\bin\g++.exe';$windres=Tool 'w64devkit\bin\windres.exe'
    $env:PATH="$(Split-Path -Parent $cxx);$(Split-Path -Parent $cmake);$(Split-Path -Parent $ninja);$env:PATH"
    if(!$MainOnly){
        if(!$VulkanInclude -or !$VulkanLibrary){throw 'Full builds require VulkanInclude and VulkanLibrary'}
        $headers=CMake-Path ([IO.Path]::GetFullPath($VulkanInclude))
        $library=CMake-Path ([IO.Path]::GetFullPath($VulkanLibrary))
        if(!(Test-Path -LiteralPath "$headers/vulkan/vulkan.h") -or !(Test-Path -LiteralPath $library)){throw 'Vulkan development headers/import library missing'}
        if(!(Test-Path -LiteralPath (Join-Path $sourceRoot 'third_party\ncnn\glslang\CMakeLists.txt'))){throw 'Pinned glslang source must be installed before input freeze.'}
    }
    Run-Native $cxx @('--version');Run-Native $cmake @('--version');Run-Native $ninja @('--version')
    $before=@(Input-Hashes)
    $beforePath=Join-Path $EvidenceRoot ("inputs-before-$stamp.json")
    Save-Json $beforePath $before
    $record.before=[pscustomobject]@{Path=$beforePath;SHA256=(Get-FileHash -LiteralPath $beforePath).Hash;Count=$before.Count}
    Save-Json $recordPath $record
    $common=@('-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninja","-DCMAKE_C_COMPILER=$cc","-DCMAKE_CXX_COMPILER=$cxx","-DCMAKE_RC_COMPILER=$windres",'-DCMAKE_POLICY_VERSION_MINIMUM=3.5','-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
    $cpuBuild=Join-Path $BuildRoot 'cpu';$gpuBuild=Join-Path $BuildRoot 'gpu'
    Run-Native $cmake (@('-S',(CMake-Path $sourceRoot),'-B',(CMake-Path $cpuBuild),'-DRCG_GPU=OFF')+$common)
    if($MainOnly){
        Run-Native $cmake @('--build',(CMake-Path $cpuBuild),'--target','RealCUGAN','--parallel',"$Parallel")
        $products=@(@{Source=(Join-Path $cpuBuild 'RealCUGAN.exe');Name='RealCUGAN.exe'})
    }else{
        Run-Native $cmake @('--build',(CMake-Path $cpuBuild),'--target','realcugan-worker','--parallel',"$Parallel")
        Run-Native $cmake (@('-S',(CMake-Path $sourceRoot),'-B',(CMake-Path $gpuBuild),'-DRCG_GPU=ON',"-DVulkan_INCLUDE_DIR=$headers","-DVulkan_LIBRARY=$library")+$common)
        Run-Native $cmake @('--build',(CMake-Path $gpuBuild),'--target','RealCUGAN','realcugan-gpu-worker','--parallel',"$Parallel")
        $products=@(
            @{Source=(Join-Path $cpuBuild 'realcugan-worker.exe');Name='realcugan-worker.exe'},
            @{Source=(Join-Path $gpuBuild 'realcugan-gpu-worker.exe');Name='realcugan-gpu-worker.exe'},
            @{Source=(Join-Path $gpuBuild 'RealCUGAN.exe');Name='RealCUGAN.exe'}
        )
    }
    $after=@(Input-Hashes)
    $afterPath=Join-Path $EvidenceRoot ("inputs-after-$stamp.json")
    Save-Json $afterPath $after
    $record.after=[pscustomobject]@{Path=$afterPath;SHA256=(Get-FileHash -LiteralPath $afterPath).Hash;Count=$after.Count}
    if(($before|ConvertTo-Json -Depth 4 -Compress) -cne ($after|ConvertTo-Json -Depth 4 -Compress)){throw 'Frozen build inputs changed. Outputs are NOT valid candidates.'}
    $record.inputs_unchanged=$true
    $staging=Join-Path $EvidenceRoot ("candidate-$stamp")
    $backup=Join-Path $EvidenceRoot ("previous-products-$stamp")
    New-Item -ItemType Directory -Path $staging,$backup -Force | Out-Null
    foreach($product in $products){
        Copy-Item -LiteralPath $product.Source -Destination (Join-Path $staging $product.Name)
        $old=Join-Path $packageRoot $product.Name
        if(Test-Path -LiteralPath $old){Copy-Item -LiteralPath $old -Destination (Join-Path $backup $product.Name)}
    }
    try{
        foreach($product in $products){Copy-Item -LiteralPath (Join-Path $staging $product.Name) -Destination (Join-Path $packageRoot $product.Name) -Force}
        foreach($product in $products){
            $published=Join-Path $packageRoot $product.Name
            $hash=(Get-FileHash -LiteralPath $product.Source).Hash.ToLowerInvariant()
            if((Get-FileHash -LiteralPath $published).Hash.ToLowerInvariant() -ne $hash){throw 'Published product hash mismatch'}
            $record.artifacts+=@{File=$product.Name;BuildPath=$product.Source;Bytes=(Get-Item -LiteralPath $published).Length;SHA256=$hash}
        }
    }catch{
        foreach($product in $products){
            $old=Join-Path $backup $product.Name;$published=Join-Path $packageRoot $product.Name
            if(Test-Path -LiteralPath $old){Copy-Item -LiteralPath $old -Destination $published -Force}
            elseif(Test-Path -LiteralPath $published){Remove-Item -LiteralPath $published}
        }
        throw
    }
    $generated=Get-ChildItem -LiteralPath $BuildRoot -Recurse -File | Where-Object {$_.Extension -in @('.h','.hpp','.cpp','.c','.inc','.spv','.a') -or $_.Name -in @('build.ninja','CMakeCache.txt','compile_commands.json')} | Sort-Object FullName | ForEach-Object {
        [pscustomobject]@{Path=[IO.Path]::GetRelativePath($BuildRoot,$_.FullName).Replace('\','/');Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
    }
    Save-Json (Join-Path $EvidenceRoot ("generated-and-libraries-$stamp.json")) @($generated)
    foreach($kind in @('cpu','gpu')){
        foreach($name in @('CMakeCache.txt','compile_commands.json','build.ninja','.ninja_log')){
            $path=Join-Path (Join-Path $BuildRoot $kind) $name
            if(Test-Path -LiteralPath $path){Copy-Item -LiteralPath $path -Destination (Join-Path $EvidenceRoot ("$kind-$stamp-$name"))}
        }
    }
    $record.status='built-candidate-not-run'
}catch{
    $record.status='failed-or-invalid';$record.error=$_.Exception.Message
    throw
}finally{
    $record.ended=Get-Date -Format o
    Save-Json $recordPath $record
}
Write-Output "Built candidates only; no GUI/worker/test execution. Evidence: $recordPath"
