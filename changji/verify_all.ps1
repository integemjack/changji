# 把"这份代码还是好的吗"这个问题一次问完。
#
# 四件事，顺序是**按发现问题的快慢排的**，前面的先跑：
#   1. 默认构建（sd.cpp，不带进程内配音）
#   2. 单元测试
#   3. llama 构建（CHANGJI_LLAMA=ON）
#   4. webapp 的测试和客户端构建
#
# **原来这里还有两步对拍**（起 Python 后端和 C++ 后端逐条比响应），
# 外加一步"配置模板 Python 还读不读得动"。阶段 8 删掉 Python 引擎之后
# 没有另一侧可比了，那三步跟着走。剩下的安全网是 cpp/tests/golden/ 里
# 那批 JSON——当年由 Python 真实函数导出、现在冻在版本库里，
# 单元测试直接读文件。
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
# ⚠️ 这个文件必须带 UTF-8 BOM：不带的话 PowerShell 5.1 按本地代码页读，
# 里面的中文全变成乱码，而报错信息不会提到编码。

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
        Write-Host "  只有 $n 条用例——多半是有测试文件没编进 CMakeLists" -ForegroundColor Red
        return $false
    }
    return $true
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
