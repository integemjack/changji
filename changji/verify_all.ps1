# 把"这份代码还是好的吗"这个问题一次问完。
#
# 五件事，顺序是**按发现问题的快慢排的**，前面的先跑：
#   1. 默认构建（sd.cpp，不带进程内配音）
#   2. 单元测试
#   3. 配置模板 Python 还读不读得动（迁移期间两边共用一份配置）
#   4. 对拍（会自己起四个进程：Python 后端、C++ 后端、两个假大模型）
#   5. llama 构建（CHANGJI_LLAMA=ON）+ 拿它再跑一遍对拍
#   6. webapp 的测试和客户端构建
#
# 为什么值得有这个脚本：
#
# **一、vcvars 那一步最容易忘。** 不进 MSVC 环境直接 cmake --build，
# 报的是 `fatal error C1083: 无法打开包括文件: "algorithm"`——
# 那看着像代码问题，其实只是 INCLUDE 没设。
#
# **二、"全绿"要有一个统一的说法。** 这几样分散在三个目录、两种语言，
# 手动一条条敲很容易漏掉一样然后以为都过了。漏掉的那一样往往正是
# 会出问题的那一样。
#
# 用法（在 changji/ 下）：
#     powershell -ExecutionPolicy Bypass -File verify_all.ps1
#     powershell ... -File verify_all.ps1 -Quick    # 跳过 llama 构建那一档
#
# ⚠️ 这个文件必须带 UTF-8 BOM，理由见 cpp/tools/duiping.ps1 的文件头。

param(
    # 跳过 CHANGJI_LLAMA 那一档。它要多编一份 llama.cpp，冷启动几分钟；
    # 只改了接口层的时候不必每次都等。
    [switch]$Quick
)

$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$cpp = Join-Path $root 'cpp'
$webapp = Join-Path $root 'webapp'
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'

$results = @()
$failed = $false

function Step($name, [scriptblock]$body) {
    Write-Host ""
    Write-Host "==== $name ====" -ForegroundColor Cyan
    $ok = & $body
    $script:results += [pscustomobject]@{ 名称 = $name; 结果 = $(if ($ok) { '过' } else { '没过' }) }
    if (-not $ok) { $script:failed = $true }
}

# ---- MSVC 环境 ----
# 见文件头第一条。cmd /c 出来的是一整份环境，逐条塞回当前会话。
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    Write-Host "找不到 vcvars64.bat：$vcvars" -ForegroundColor Red
    Write-Host "装的不是 Build Tools 的话路径不一样，改这一行。"
    exit 2
}
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] }
}

Step "默认构建" {
    $out = & $cmake --build (Join-Path $cpp 'build') 2>&1
    $bad = $out | Select-String -Pattern 'error C|error LNK|FAILED:'
    if ($bad) { $bad | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }; return $false }
    Write-Host "  编过了"
    return $true
}

Step "单元测试" {
    $exe = Join-Path $cpp 'build\changji_tests.exe'
    if (-not (Test-Path $exe)) { Write-Host "  没有 $exe" -ForegroundColor Red; return $false }
    $out = & $exe 2>&1
    $line = $out | Select-String -Pattern 'test cases:'
    Write-Host "  $line"
    if ($LASTEXITCODE -ne 0) { return $false }

    # **也要看条数。** 退出码为 0 只说明"跑到的都过了"——
    # 有人从 CMakeLists 里漏掉一个测试文件，或者整个 tests/unit 没编进去，
    # 剩下的照样全过、退出码照样 0。和对拍那边是同一类失败：
    # 数字悄悄掉下来，而结论不变。
    #
    # 这个下限只用来拦断崖式的下跌，不用跟着每次加用例改。
    if ($line -notmatch 'test cases:\s*(\d+)') { return $false }
    $n = [int]$matches[1]
    if ($n -lt 400) {
        Write-Host "  只有 $n 条用例，正常是 480 多条——多半是有测试文件没编进去" -ForegroundColor Red
        return $false
    }
    return $true
}

Step "配置模板两边都读得动" {
    # 迁移期间两个后端共用一份用户配置。模板里出现一个 Python 不认的键，
    # **Python 整份加载失败、后端起不来**——今天断过一次，症状是对拍里
    # 152 条全变成"Python 侧：连不上"，看着像端口问题。
    $py = Join-Path $root '.venv\Scripts\python.exe'
    if (-not (Test-Path $py)) { Write-Host "  没有 .venv，跳不了也测不了" -ForegroundColor Yellow; return $false }
    $out = & $py (Join-Path $cpp 'tools\check_template_compat.py') 2>&1
    $out | ForEach-Object { Write-Host "  $_" }
    return ($LASTEXITCODE -eq 0)
}

Step "对拍（默认构建）" {
    $out = powershell -ExecutionPolicy Bypass -File (Join-Path $cpp 'tools\duiping.ps1') 2>&1
    $line = $out | Select-String -Pattern '一致 \d'
    if (-not $line) {
        Write-Host "  对拍没跑起来，最后几行：" -ForegroundColor Red
        $out | Select-Object -Last 6 | ForEach-Object { Write-Host "    $_" }
        return $false
    }
    Write-Host "  $line"
    # 「不同」不是 0 就算没过；「跳过」也要看一眼，跳过等于放弃检查。
    #
    # **还要看条数。** "不同 0" 出自 20 条和出自 165 条完全是两回事——
    # 对拍工具的 --golden 默认是相对路径，工作目录不对时语料一份都读不到，
    # 那些模式静静地不跑，结论照样是"不同 0、跳过 0"、退出码 0。
    # 第一次跑这个脚本就是这么骗过我的（报了 20 条，差点当成过了）。
    if ($line -notmatch '一致 (\d+)') { return $false }
    $n = [int]$matches[1]
    if ($n -lt 150) {
        Write-Host "  只跑了 $n 条，正常是 160 多条——多半是语料没读到" -ForegroundColor Red
        return $false
    }
    return ($line -match '不同 0' -and $line -match '跳过 0')
}

if (-not $Quick) {
    Step "llama 构建" {
        $dir = Join-Path $cpp 'build_llama'
        if (-not (Test-Path $dir)) {
            Write-Host "  第一次要先配置（会拉 llama.cpp，几分钟）"
            & $cmake -S $cpp -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHANGJI_LLAMA=ON 2>&1 | Out-Null
        }
        $out = & $cmake --build $dir --target changji 2>&1
        $bad = $out | Select-String -Pattern 'error C|error LNK|FAILED:'
        if ($bad) { $bad | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }; return $false }
        Write-Host "  编过了"
        return $true
    }

    Step "对拍（llama 构建）" {
        $out = powershell -ExecutionPolicy Bypass -File (Join-Path $cpp 'tools\duiping.ps1') `
            -CppExe 'build_llama\changji.exe' 2>&1
        $line = $out | Select-String -Pattern '一致 \d'
        if (-not $line) { Write-Host "  没跑起来" -ForegroundColor Red; return $false }
        Write-Host "  $line"
        if ($line -notmatch '一致 (\d+)' -or [int]$matches[1] -lt 150) {
            Write-Host "  条数不对，多半是语料没读到" -ForegroundColor Red
            return $false
        }
        return ($line -match '不同 0' -and $line -match '跳过 0')
    }
}

Step "webapp" {
    Push-Location $webapp
    try {
        $t = npm test 2>&1
        $tl = $t | Select-String -Pattern 'Tests  '
        Write-Host "  $tl"
        if ($LASTEXITCODE -ne 0) { return $false }
        $b = npm run build 2>&1
        if ($LASTEXITCODE -ne 0) {
            $b | Select-Object -Last 5 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
            return $false
        }
        Write-Host "  客户端编过了"
        return $true
    } finally { Pop-Location }
}

Write-Host ""
Write-Host "==== 汇总 ====" -ForegroundColor Cyan
$results | Format-Table -AutoSize | Out-String | Write-Host
if ($failed) {
    Write-Host "有没过的。" -ForegroundColor Red
    exit 1
}
Write-Host "全过了。" -ForegroundColor Green
exit 0
