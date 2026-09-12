@echo off
REM 本机编译 + 跑单元测试。
REM
REM **只能从 PowerShell 调，不能从 Bash 调**：vcvars64.bat 要靠 cmd 的环境
REM 变量继承，Bash 起的子 shell 拿不到，表现是 cl.exe 找不到。
REM
REM -DCHANGJI_SSL=OFF 必须显式写：build/CMakeCache.txt 里如果残留着 ON，
REM ninja 会自己触发 reconfigure，然后死在 OpenSSL 找不到上——报的是
REM 配置错误，跟这次改的代码毫无关系，查起来很费劲。
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0cpp"
cmake -S . -B build -DCHANGJI_SSL=OFF >nul
if errorlevel 1 exit /b 1

cmake --build build --target changji_tests
if errorlevel 1 exit /b 1

build\changji_tests.exe %*
