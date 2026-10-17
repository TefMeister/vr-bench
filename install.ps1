# install.ps1 -- copies the VR Bench layer from dist\ to %LOCALAPPDATA%\Programs\VRBench and writes its two manifests
# there with the real path. It REGISTERS NOTHING: the Lanes plugin registers the layer per run, and only the copy
# that matches the game (32-bit or 64-bit), then unregisters it again. Run again after a rebuild.
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-From <folder with the built files>]
param([string]$From = "")
# from a source folder the built files are in dist\; from the download zip they sit next to this script
if ($From -eq "") { $From = if (Test-Path (Join-Path $PSScriptRoot "dist\vrbench_layer.dll")) { Join-Path $PSScriptRoot "dist" } else { $PSScriptRoot } }
$ErrorActionPreference = "Stop"
$to = Join-Path $env:LOCALAPPDATA "Programs\VRBench"
New-Item -ItemType Directory -Force $to | Out-Null

function ManifestText([string]$name, [string]$dll, [string]$bits) {
    # the manifest names the DLL by its full path, so it works wherever the folder was put
    $escaped = $dll.Replace("\", "\\")
    return "{`n  `"file_format_version`": `"1.0.0`",`n  `"api_layer`": {`n    `"name`": `"$name`",`n" +
           "    `"library_path`": `"$escaped`",`n    `"api_version`": `"1.0`",`n    `"implementation_version`": `"1`",`n" +
           "    `"description`": `"VR Bench ($bits): pretend hands and buttons for a VR game, and the look-at-this mark`",`n" +
           "    `"disable_environment`": `"DISABLE_VRBENCH_LAYER`"`n  }`n}`n"
}

$copied = @()
foreach ($pair in @(@("vrbench_layer.dll", "vrbench_layer.json", "XR_APILAYER_LANES_vr_bench", "64-bit"),
                    @("vrbench_layer32.dll", "vrbench_layer32.json", "XR_APILAYER_LANES_vr_bench32", "32-bit"))) {
    $dll, $json, $name, $bits = $pair
    $src = Join-Path $From $dll
    if (-not (Test-Path $src)) { continue }
    Copy-Item $src (Join-Path $to $dll) -Force
    [IO.File]::WriteAllText((Join-Path $to $json), (ManifestText $name (Join-Path $to $dll) $bits))
    $copied += $bits
}
if ($copied.Count -eq 0) { throw "no vrbench_layer*.dll found in $From" }
Write-Output "Installed to $to ($($copied -join ', ')); the plugin switches it on per run."
