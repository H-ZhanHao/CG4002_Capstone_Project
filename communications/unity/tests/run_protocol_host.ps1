$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$project = Join-Path $repoRoot 'communications/unity/Ultra96VisualizerDemo'
$output = Join-Path $repoRoot '.build/unity_host'
New-Item -ItemType Directory -Force $output | Out-Null
$dotnetRoot = Split-Path (Get-Command dotnet).Source
$sdk = Get-ChildItem (Join-Path $dotnetRoot 'sdk') -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1
$compiler = Join-Path $sdk.FullName 'Roslyn/bincore/csc.dll'
[xml]$csproj = Get-Content -LiteralPath (Join-Path $project 'CG4002.Visualizer.csproj')
$references = @{}
foreach ($reference in $csproj.Project.ItemGroup.Reference) {
    if (-not $reference.HintPath) { continue }
    $path = [string]$reference.HintPath
    if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $project $path }
    if (Test-Path -LiteralPath $path) { $references[[IO.Path]::GetFileName($path)] = $path }
}
foreach ($required in @('netstandard.dll', 'nunit.framework.dll', 'Newtonsoft.Json.dll', 'UnityEngine.CoreModule.dll')) {
    if (-not $references.ContainsKey($required)) { throw "Missing installed Unity reference: $required" }
}
$arguments = @('/nostdlib+', '/langversion:9.0', '/target:exe', "/out:$output/ProtocolHost.dll")
foreach ($path in $references.Values) { $arguments += "/reference:`"$path`"" }
foreach ($source in Get-ChildItem (Join-Path $project 'Assets/Scripts/Networking') -Filter '*.cs') { $arguments += "`"$($source.FullName)`"" }
foreach ($source in Get-ChildItem (Join-Path $project 'Assets/Scripts/UI') -Filter '*.cs') { $arguments += "`"$($source.FullName)`"" }
foreach ($source in Get-ChildItem (Join-Path $project 'Assets/Tests/EditMode') -Filter '*.cs') { $arguments += "`"$($source.FullName)`"" }
$arguments += "`"$(Join-Path $PSScriptRoot 'ProtocolHost.cs')`""
$response = Join-Path $output 'compile.rsp'
[IO.File]::WriteAllLines($response, $arguments)
& dotnet $compiler /noconfig "@$response"
if ($LASTEXITCODE -ne 0) { throw 'Managed Unity source compilation failed' }
foreach ($name in @('nunit.framework.dll', 'Newtonsoft.Json.dll', 'UnityEngine.CoreModule.dll', 'UnityEngine.SharedInternalsModule.dll', 'UnityEngine.dll')) {
    if ($references.ContainsKey($name)) { Copy-Item -LiteralPath $references[$name] -Destination $output }
}
$major = ([version]$sdk.Name).Major
$runtime = @{ runtimeOptions = @{ tfm = "net$major.0"; framework = @{ name = 'Microsoft.NETCore.App'; version = "$major.0.0" } } }
[IO.File]::WriteAllText((Join-Path $output 'ProtocolHost.runtimeconfig.json'), ($runtime | ConvertTo-Json -Depth 4))
& dotnet (Join-Path $output 'ProtocolHost.dll')
if ($LASTEXITCODE -ne 0) { throw 'Managed protocol test cases failed' }
