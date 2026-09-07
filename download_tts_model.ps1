# 下载 Qwen3-TTS 权重（断点续传 + 自动重试）
# 节点自带的下载在国内容易断在半路，且断了之后它看到目录存在就不再重下。
$ErrorActionPreference = 'Continue'
$D = "E:\AI短剧\ComfyUI\models\TTS\qwen3_tts\Qwen3-TTS-12Hz-1.7B-Base"
$B = "https://hf-mirror.com/Qwen/Qwen3-TTS-12Hz-1.7B-Base/resolve/main"

$files = @(
  @{ rel="model.safetensors";                  url="$B/model.safetensors" },
  @{ rel="speech_tokenizer\model.safetensors"; url="$B/speech_tokenizer/model.safetensors" },
  @{ rel="speech_tokenizer\config.json";       url="$B/speech_tokenizer/config.json" },
  @{ rel="speech_tokenizer\configuration.json";url="$B/speech_tokenizer/configuration.json" },
  @{ rel="speech_tokenizer\preprocessor_config.json"; url="$B/speech_tokenizer/preprocessor_config.json" },
  @{ rel="tokenizer_config.json";              url="$B/tokenizer_config.json" }
)

function Get-RemoteSize($url) {
  $h = & curl.exe -sSIL -m 40 $url 2>$null
  $m = $h | Select-String -Pattern '^\s*content-length:\s*(\d+)' -AllMatches
  if ($m) { return [int64]($m[-1].Matches[0].Groups[1].Value) }
  return 0
}

foreach ($f in $files) {
  $out = Join-Path $D $f.rel
  New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
  $expected = 0
  for ($h = 1; $h -le 4 -and $expected -le 0; $h++) {
    $expected = Get-RemoteSize $f.url
    if ($expected -le 0) { Start-Sleep 3 }
  }
  if ($expected -le 0) { Write-Host "[SKIP] $($f.rel) 取不到大小"; continue }
  Write-Host "--- $($f.rel)  目标 $([math]::Round($expected/1MB)) MB ---"
  for ($try = 1; $try -le 60; $try++) {
    $have = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
    if ($have -ge $expected) { break }
    Write-Host "  [$try] $([math]::Round($have/1MB)) / $([math]::Round($expected/1MB)) MB"
    & curl.exe -sSL --retry 8 --retry-all-errors --retry-delay 3 `
        --connect-timeout 30 --speed-limit 10240 --speed-time 60 `
        -C - -o $out $f.url 2>$null
    Start-Sleep 2
  }
  $have = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
  if ($have -ge $expected) { Write-Host "[OK] $($f.rel) $([math]::Round($have/1MB)) MB" }
  else { Write-Host "[FAIL] $($f.rel) $have / $expected" }
}
Write-Host "=== 完成 ==="
Get-ChildItem $D -Recurse -Filter *.safetensors | Select-Object Name,@{n='MB';e={[math]::Round($_.Length/1MB)}} | Format-Table -AutoSize
