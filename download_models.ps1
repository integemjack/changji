# Wan 2.2 模型下载：断点续传 + 自动重试（直连 huggingface.co）
$ErrorActionPreference = 'Continue'
$M = "E:\AI短剧\ComfyUI\models"
$B = "https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files"

$files = @(
  @{ dir="diffusion_models"; name="wan2.2_ti2v_5B_fp16.safetensors";        url="$B/diffusion_models/wan2.2_ti2v_5B_fp16.safetensors" },
  @{ dir="vae";              name="wan2.2_vae.safetensors";                  url="$B/vae/wan2.2_vae.safetensors" },
  @{ dir="text_encoders";    name="umt5_xxl_fp8_e4m3fn_scaled.safetensors";  url="$B/text_encoders/umt5_xxl_fp8_e4m3fn_scaled.safetensors" }
)

function Get-RemoteSize($url) {
  $h = & curl.exe -sSIL -m 40 $url 2>$null
  $lens = $h | Select-String -Pattern '^\s*content-length:\s*(\d+)' -AllMatches
  if ($lens) { return [int64]($lens[-1].Matches[0].Groups[1].Value) }
  return 0
}

foreach ($f in $files) {
  $out = Join-Path $M "$($f.dir)\$($f.name)"
  $expected = 0
  for ($h = 1; $h -le 5 -and $expected -le 0; $h++) { $expected = Get-RemoteSize $f.url; if ($expected -le 0) { Start-Sleep 3 } }
  if ($expected -le 0) { Write-Host "[SKIP] cannot resolve size for $($f.name)"; continue }
  Write-Host "--- $($f.name) : target $([math]::Round($expected/1MB)) MB ---"

  for ($try = 1; $try -le 80; $try++) {
    $have = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
    if ($have -ge $expected) { break }
    if ($have -gt 0) { Write-Host "[$try] resume $([math]::Round($have/1MB)) / $([math]::Round($expected/1MB)) MB" }
    else { Write-Host "[$try] start 0 / $([math]::Round($expected/1MB)) MB" }
    & curl.exe -sSL --retry 5 --retry-all-errors --retry-delay 3 --connect-timeout 30 --speed-limit 10240 --speed-time 60 -C - -o $out $f.url 2>$null
    $after = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
    if ($after -lt $have) { Write-Host "  !! shrank $have -> $after, keeping larger is impossible; restarting fresh"; Remove-Item $out -Force -ErrorAction SilentlyContinue }
    Start-Sleep 2
  }

  $have = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
  if ($have -ge $expected) { Write-Host "[OK] $($f.name) $([math]::Round($have/1MB)) MB" }
  else { Write-Host "[FAIL] $($f.name) $have / $expected" }
}

Write-Host "=== DOWNLOAD SCRIPT FINISHED ==="
Get-ChildItem "$M\diffusion_models","$M\vae","$M\text_encoders" -Filter *.safetensors -ErrorAction SilentlyContinue | Select-Object Name,@{n='MB';e={[math]::Round($_.Length/1MB)}} | Format-Table -AutoSize
