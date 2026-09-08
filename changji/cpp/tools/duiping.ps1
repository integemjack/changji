# 起齐对拍要的四个进程，跑一轮，然后收干净。
#
# 四个：两个假大模型（每一侧一个，不共用队列——/api/plan 一次要问两遍，
# 共用的话第二个后端拿到的是第一个剩下的，永远错位），
# 加 Python 后端和 C++ 后端。
#
# 两条踩过的坑，都写在这儿免得再踩：
#
# **一、这个文件必须带 UTF-8 BOM。** PowerShell 5.1 读没有 BOM 的 .ps1
# 时按当前 ANSI 代码页解，中文字符串字面量会变成乱码——
# 第一次跑的时候项目路径变成了"椤圭洰_闆ㄥ澶╁彴"，然后每一条都是
# "这里不是一个项目目录"。为了不再依赖这一点，下面的项目路径改成
# **扫出来的**，不写字面量。
#
# **二、两侧的 LLM 地址必须不同**，所以不能靠当前会话的环境变量——
# 那是一份、两个进程共用。这里给每个后端写一个临时 .cmd，
# 在里面 set 完再拉起进程。
#
# 用法（在 changji/cpp 下）：
#     powershell -ExecutionPolicy Bypass -File tools\duiping.ps1
#     powershell ... -File tools\duiping.ps1 -Case plan -ShowRequests

param(
    [string]$Case = "",
    [switch]$ShowRequests,
    [int]$CppPort = 8123,
    [int]$PyPort = 8124,
    [int]$LlmPyPort = 8125,
    [int]$LlmCppPort = 8126,
    # 拿哪个 C++ 二进制去对拍。默认是主构建；开了 CHANGJI_LLAMA 的那份
    # 放在 build_llama 下，用 -CppExe build_llama\changji.exe 指过去，
    # 可以验"链进 llama.cpp 之后接口行为一个字都没变"。
    [string]$CppExe = "build\changji.exe"
)

$ErrorActionPreference = "Stop"
$cpp = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $cpp
$py = Join-Path $root ".venv\Scripts\python.exe"
$work = Join-Path $cpp "build\fake_llm"

# 项目路径扫出来，不写字面量：见文件头第一条。
$golden = Join-Path $cpp "tests\golden"
$project = (Get-ChildItem $golden -Directory |
            Where-Object { Test-Path (Join-Path $_.FullName "project.json") } |
            Select-Object -First 1).FullName
if (-not $project) { Write-Error "在 $golden 下找不到带 project.json 的目录"; exit 2 }

function Stop-All {
    Get-Process changji -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
        Where-Object { $_.CommandLine -like "*serve_python*" -or $_.CommandLine -like "*fake_llm*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

Stop-All
Start-Sleep -Seconds 1
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue

Start-Process -FilePath $py -ArgumentList "cpp\tools\fake_llm.py",$LlmPyPort,(Join-Path $work "py") -WorkingDirectory $root -WindowStyle Hidden
Start-Process -FilePath $py -ArgumentList "cpp\tools\fake_llm.py",$LlmCppPort,(Join-Path $work "cpp") -WorkingDirectory $root -WindowStyle Hidden
Start-Sleep -Seconds 2

# 每个后端一个临时 .cmd：在里面 set 自己的 LLM 地址再拉起进程。
# 用文件而不是 `cmd /c "set A && B"`：那条命令行要经过 PowerShell 和 cmd
# 两次转义，很容易被拆坏（serve_python.py 就是这么诞生的）。
$pyCmd = Join-Path $work "run_py.cmd"
$cppCmd = Join-Path $work "run_cpp.cmd"
New-Item -ItemType Directory -Force -Path $work | Out-Null
Set-Content -Path $pyCmd -Encoding ASCII -Value @(
    "@echo off",
    "set CHANGJI_LLM_BASE_URL=http://127.0.0.1:$LlmPyPort/v1",
    "cd /d `"$root`"",
    "`"$py`" cpp\tools\serve_python.py $PyPort"
)
Set-Content -Path $cppCmd -Encoding ASCII -Value @(
    "@echo off",
    "set CHANGJI_LLM_BASE_URL=http://127.0.0.1:$LlmCppPort/v1",
    # **让 C++ 也走 ComfyUI 那条路。** 不设的话它默认 engine=sd，
    # 出图出片走进程内的 sd.cpp，根本不加载 ComfyUI 工作流——
    # 那时候推理层那一条比的是两条不同的路，没有意义。
    # 两边都连不上 ComfyUI，但至少走的是同一条。
    "set CHANGJI_MODELS_ENGINE=comfy",
    "cd /d `"$cpp`"",
    "`"$cpp\$CppExe`" --port $CppPort"
)
Start-Process -FilePath $pyCmd -WindowStyle Hidden
Start-Process -FilePath $cppCmd -WindowStyle Hidden
Start-Sleep -Seconds 8

$argv = @("--cpp","http://127.0.0.1:$CppPort","--python","http://127.0.0.1:$PyPort",
          "--project",$project,"--llm-work",$work)
if ($Case) { $argv += @("--case",$Case) }
if ($ShowRequests) { $argv += "--verbose" }

& (Join-Path $cpp "build\changji_compat.exe") @argv
$code = $LASTEXITCODE

Stop-All
exit $code
