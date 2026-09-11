# 下载 C++ 后端（sd.cpp）出视频要的三份权重，GGUF 格式。
#
# **和当初那份 download_models.ps1 不是一回事**（那个 2026-09-11 随 ComfyUI
#   一起删了）。它下的是 fp16 的 safetensors，
# 给 ComfyUI 用；sd.cpp 吃的是 GGUF。两条路的权重格式不通用，
# 不要互相替代。（那一份还指着 E:\AI短剧\... —— 这台机器上没有 E 盘。）
#
# 体积差得很远，这是它值得单独写一个脚本的原因：
#
#   fp16 safetensors   9536 + 1344 + 6423 = 16.9 GB
#   GGUF Q4_K_M        3274 + 1344 + 3485 =  7.9 GB
#   GGUF Q3_K_M        2429 + 1344 + 2913 =  6.5 GB
#
# 三份分别是：
#   扩散主模型  Wan2.2-TI2V-5B   → [models].video
#   VAE         wan2.2_vae       → [models].video_vae（这一份没有 GGUF，用 safetensors）
#   文本编码器  umt5-xxl         → [models].video_text_encoder
#
# 用法（在仓库根目录）：
#     powershell -ExecutionPolicy Bypass -File download_wan_gguf.ps1
#     powershell ... -File download_wan_gguf.ps1 -Quant Q3_K_M      # 盘紧的时候
#     powershell ... -File download_wan_gguf.ps1 -Dest "D:\models" -Official
#
# ⚠️ 这个文件必须带 UTF-8 BOM。PowerShell 5.1 读没有 BOM 的 .ps1 时按当前
#    ANSI 代码页解，中文字符串会变成乱码。

param(
    [string]$Dest = "$env:LOCALAPPDATA\changji\models",

    # 量化档。**6 GB 显存上跑得动的是 Q4_K_M 这一档**；盘实在不够就 Q3_K_M，
    # 画质会掉一些但能跑起来。Q8_0/BF16 这台放不下，没列。
    [ValidateSet("Q4_K_M", "Q3_K_M")]
    [string]$Quant = "Q4_K_M",

    [switch]$Official
)

$ErrorActionPreference = 'Continue'
$h = if ($Official) { "https://huggingface.co" } else { "https://hf-mirror.com" }

$files = @(
    @{ what = "扩散主模型";  key = "video";
       name = "Wan2.2-TI2V-5B-$Quant.gguf";
       url  = "$h/QuantStack/Wan2.2-TI2V-5B-GGUF/resolve/main/Wan2.2-TI2V-5B-$Quant.gguf" },
    @{ what = "VAE";        key = "video_vae";
       # VAE 只有 safetensors，没有 GGUF 版。它本来就不大，不量化也还好。
       name = "wan2.2_vae.safetensors";
       url  = "$h/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/vae/wan2.2_vae.safetensors" },
    @{ what = "文本编码器";  key = "video_text_encoder";
       name = "umt5-xxl-encoder-$Quant.gguf";
       url  = "$h/city96/umt5-xxl-encoder-gguf/resolve/main/umt5-xxl-encoder-$Quant.gguf" }
)

Write-Host "镜像   $h"
Write-Host "量化   $Quant"
Write-Host "目标   $Dest"
Write-Host ""

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

function Get-RemoteSize($url) {
    $r = & curl.exe -sSIL -m 40 $url 2>$null
    $m = $r | Select-String -Pattern '^\s*content-length:\s*(\d+)' -AllMatches
    if ($m) { return [int64]($m[-1].Matches[0].Groups[1].Value) }
    return 0
}

# **先把三份的总大小问出来，和剩余空间一起摆给用户看，再动手下。**
# 下到第三份才发现盘满，前两份也白等了——断点续传救得回来，
# 但用户得先知道自己该去腾多少地方。
$total = 0
foreach ($f in $files) {
    $sz = 0
    for ($i = 1; $i -le 4 -and $sz -le 0; $i++) { $sz = Get-RemoteSize $f.url; if ($sz -le 0) { Start-Sleep 3 } }
    $f.size = $sz
    if ($sz -le 0) {
        Write-Host "[跳过] $($f.name) 取不到大小：$($f.url)" -ForegroundColor Yellow
    } else {
        Write-Host ("  {0,-12} {1,-42} {2,6} MB" -f $f.what, $f.name, [math]::Round($sz/1MB))
        $total += $sz
    }
}
Write-Host ("  {0,-12} {1,-42} {2,6} MB" -f "合计", "", [math]::Round($total/1MB))

$drive = (Get-Item $Dest).PSDrive
$have = 0
foreach ($f in $files) {
    $p = Join-Path $Dest $f.name
    if (Test-Path $p) { $have += (Get-Item $p).Length }
}
$need = $total - $have
if ($drive -and $drive.Free) {
    Write-Host ""
    Write-Host ("盘 {0}: 剩 {1} GB，还要下 {2} GB" -f $drive.Name,
        [math]::Round($drive.Free/1GB, 1), [math]::Round($need/1GB, 1))
    if ($drive.Free -lt $need) {
        Write-Host "  ⚠️ 空间不够。" -ForegroundColor Yellow
        if ($Quant -eq "Q4_K_M") { Write-Host "     换 -Quant Q3_K_M 能省约 1.4 GB。" -ForegroundColor Yellow }
        Write-Host "     或者腾地方：changji/cpp 下的 build_llama 目录是可重新生成的构建产物。" -ForegroundColor Yellow
        Write-Host "     现在还是会开始下（断点续传），Ctrl+C 可以随时停。" -ForegroundColor Yellow
    }
}
Write-Host ""

$ok = $true
foreach ($f in $files) {
    if ($f.size -le 0) { $ok = $false; continue }
    $out = Join-Path $Dest $f.name
    Write-Host "--- $($f.what) $($f.name)  目标 $([math]::Round($f.size/1MB)) MB ---"
    for ($try = 1; $try -le 200; $try++) {
        $cur = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
        if ($cur -ge $f.size) { break }
        Write-Host "  [$try] $([math]::Round($cur/1MB)) / $([math]::Round($f.size/1MB)) MB"
        & curl.exe -sSL --retry 8 --retry-all-errors --retry-delay 3 `
            --connect-timeout 30 --speed-limit 10240 --speed-time 60 `
            -C - -o $out $f.url 2>$null
        Start-Sleep 2
    }
    $cur = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
    if ($cur -ge $f.size) { Write-Host "[好了] $($f.name)" -ForegroundColor Green }
    else { Write-Host "[没下完] $($f.name) $cur / $($f.size)" -ForegroundColor Red; $ok = $false }
}

Write-Host ""
if ($ok) {
    Write-Host "三份都齐了。把下面这段贴进配置文件：" -ForegroundColor Green
    Write-Host ""
    Write-Host "[models]"
    Write-Host 'engine = "sd"'
    foreach ($f in $files) {
        Write-Host ("{0} = `"{1}`"" -f $f.key, (Join-Path $Dest $f.name))
    }
    Write-Host ""
    Write-Host "然后 changji.exe --doctor 里「本地模型」那项应该报三份都在。"
    Write-Host "出一集还要 ffmpeg：winget install Gyan.FFmpeg"
} else {
    Write-Host "有文件没下完。再跑一遍会接着下（断点续传），不会从头来。" -ForegroundColor Yellow
    exit 1
}
