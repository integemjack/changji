# 下载进程内配音（阶段 9）要的两份 GGUF 权重。
#
# 和当初那份 download_tts_model.ps1 的区别（那个 2026-09-11 随 ComfyUI 一起
# 删了）：**它下的是 safetensors，给 Python /
# ComfyUI 那条路用的**；这一份下的是 GGUF，给 C++ 进程内的 llama.cpp + mtmd
# 用。两条路的权重格式不通用，不要互相替代。
#
# 两份都要，缺一不可：
#   talker    —— 骨干（Qwen3 LM + code predictor），文字 → 12.5 Hz 码本
#   tokenizer —— 解码器（SEANet + ConvNeXt + DAC v2 + RVQ），码本 → 24 kHz 波形
#
# 下完把打印出来的两行贴进配置文件就能用。
#
# 用法（在仓库根目录）：
#     powershell -ExecutionPolicy Bypass -File download_tts_gguf.ps1
#     powershell ... -File download_tts_gguf.ps1 -Dest "D:\models" -Official
#     powershell ... -File download_tts_gguf.ps1 -Variant base
#
# ⚠️ 这个文件必须带 UTF-8 BOM。PowerShell 5.1 读没有 BOM 的 .ps1 时按当前
#    ANSI 代码页解，中文字符串会变成乱码。（cpp/tools/duiping.ps1 踩过。）

param(
    # 放哪儿。默认跟着项目库走，和 /api/doctor 里"项目目录"同一个根。
    [string]$Dest = "$env:LOCALAPPDATA\changji\models",

    # 骨干模型的三个变体，选一个：
    #   customvoice —— 靠一段参考音频克隆音色。**默认选它**，因为
    #                  mtmd 那套接口的输入正好就是一个 speaker_ref，
    #                  而短剧要的是每个角色一个声音。
    #   base        —— 模型自带的几个预置音色。不用准备参考音频，
    #                  但 mtmd 的接口里没有"挑哪个预置音色"这个参数，
    #                  实际拿到的是默认那个。
    #   voicedesign —— 用文字描述生成音色。changji 目前没有地方填这段描述。
    [ValidateSet("customvoice", "base", "voicedesign")]
    [string]$Variant = "customvoice",

    # 量化档。Q4_K_M 约 1.2 GB，Q8_0 约 2.1 GB，BF16 约 3.9 GB。
    # 6 GB 显存上还要和首帧/视频模型轮换，Q4_K_M 是能塞下的那一档。
    [ValidateSet("Q4_K_M", "Q8_0", "BF16", "F32")]
    [string]$Quant = "Q4_K_M",

    # 默认走 hf-mirror（国内直连 huggingface.co 常常断在半路）。
    [switch]$Official
)

$ErrorActionPreference = 'Continue'

$repo = "Serveurperso/Qwen3-TTS-GGUF"
$host_ = if ($Official) { "https://huggingface.co" } else { "https://hf-mirror.com" }
$base = "$host_/$repo/resolve/main"

# 文件名取自仓库实际的文件清单，不是拼出来的。
$talker = "qwen-talker-1.7b-$Variant-$Quant.gguf"
$tokenizer = "qwen-tokenizer-12hz-$Quant.gguf"

$files = @(
    @{ name = $talker;    what = "骨干（talker）" },
    @{ name = $tokenizer; what = "解码器（tokenizer）" }
)

Write-Host "仓库   $repo"
Write-Host "镜像   $host_"
Write-Host "目标   $Dest"
Write-Host ""

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# **先看空间够不够再下。** 下到 90% 才发现盘满，前面那 90% 也白下了——
# 断点续传救得回来，但用户得先知道自己该去腾地方。
$drive = (Get-Item $Dest).PSDrive
if ($drive -and $drive.Free) {
    $freeGB = [math]::Round($drive.Free / 1GB, 1)
    Write-Host "盘 $($drive.Name): 剩 $freeGB GB"
    if ($drive.Free -lt 2GB) {
        Write-Host "  ⚠️ 不到 2 GB。Q4_K_M 两份加起来约 1.5 GB，会很紧。" -ForegroundColor Yellow
    }
    Write-Host ""
}

function Get-RemoteSize($url) {
    $h = & curl.exe -sSIL -m 40 $url 2>$null
    $m = $h | Select-String -Pattern '^\s*content-length:\s*(\d+)' -AllMatches
    if ($m) { return [int64]($m[-1].Matches[0].Groups[1].Value) }
    return 0
}

$ok = $true
foreach ($f in $files) {
    $url = "$base/$($f.name)"
    $out = Join-Path $Dest $f.name

    $expected = 0
    for ($h = 1; $h -le 4 -and $expected -le 0; $h++) {
        $expected = Get-RemoteSize $url
        if ($expected -le 0) { Start-Sleep 3 }
    }
    if ($expected -le 0) {
        # 取不到大小多半是文件名不对或者镜像不通。**把 URL 打出来**，
        # 否则用户只看到"取不到大小"，没法自己去浏览器里试一下。
        Write-Host "[跳过] $($f.name) 取不到大小：$url" -ForegroundColor Yellow
        $ok = $false
        continue
    }

    Write-Host "--- $($f.what) $($f.name)  目标 $([math]::Round($expected/1MB)) MB ---"
    for ($try = 1; $try -le 60; $try++) {
        $have = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
        if ($have -ge $expected) { break }
        Write-Host "  [$try] $([math]::Round($have/1MB)) / $([math]::Round($expected/1MB)) MB"
        # -C - 是断点续传。国内下大文件基本一定会断，不续传的话每次从头来。
        & curl.exe -sSL --retry 8 --retry-all-errors --retry-delay 3 `
            --connect-timeout 30 --speed-limit 10240 --speed-time 60 `
            -C - -o $out $url 2>$null
        Start-Sleep 2
    }

    $have = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
    if ($have -ge $expected) {
        Write-Host "[好了] $($f.name) $([math]::Round($have/1MB)) MB" -ForegroundColor Green
    } else {
        Write-Host "[没下完] $($f.name) $have / $expected" -ForegroundColor Red
        $ok = $false
    }
}

Write-Host ""
if ($ok) {
    Write-Host "两份都齐了。把下面这段贴进配置文件（Windows 上是" -ForegroundColor Green
    # **是 LOCALAPPDATA 不是 APPDATA。** 第一版写错了：写进 Roaming 的话
    # changji 根本不读，症状是"配置贴了还是说模型没配"。
    # 实际路径可以用 changji.exe --init-config 打出来确认。
    Write-Host "$env:LOCALAPPDATA\changji\config.toml）：" -ForegroundColor Green
    Write-Host ""
    Write-Host "[tts]"
    Write-Host 'backend = "local"'
    Write-Host ""
    Write-Host "[models]"
    Write-Host "tts = `"$(Join-Path $Dest $talker)`""
    Write-Host "tts_decoder = `"$(Join-Path $Dest $tokenizer)`""
    Write-Host ""
    Write-Host "然后 changji.exe --doctor 里那项「进程内配音」应该报两份模型都在。"
    Write-Host ""
    Write-Host "音色：把角色的 voice_id 填成一段人声片段的路径，模型照着它念。"
    Write-Host "      只认 wav / mp3 / flac（mtmd 那边走 miniaudio）。不填就用默认音色。"
    Write-Host ""
    Write-Host "⚠️ 要用 CHANGJI_LLAMA=ON 编出来的二进制，默认构建没编进程内配音。"
} else {
    Write-Host "有文件没下完。再跑一遍会接着下（断点续传），不会从头来。" -ForegroundColor Yellow
    exit 1
}
