# changji C++ 后端

方案见 [../docs/C++重构方案.md](../docs/C++重构方案.md)。

当前进度：**阶段 0（骨架）**。只有 `/api/health`、`/api/doctor` 和一个
WebSocket 端点，目的是把工具链和跨平台构建先跑通。

> **下面这套 MinGW 的构建方式是过渡状态。** 方案定了在阶段 1 之前切到
> MSVC——sd.cpp 和 llama.cpp 的 CUDA 后端要过 nvcc，而 nvcc 在 Windows 上
> 只支持 MSVC。切换之后本文档里静态链接和中文路径这两节的结论都要重测，
> 见方案第四节的前置验证工程。

## 构建

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

依赖全部由 CMake 的 FetchContent 自动拉取，不需要 vcpkg 或 conan：
nlohmann/json、toml++、cpp-httplib、asio、Crow。首次配置要下载几个仓库，
大约一两分钟。

### ⚠️ Windows：路径里不能有中文

这是本项目实测踩到的坑，会浪费你一下午：

源码放在 `E:\AI短剧\` 下时，CMake 生成的子构建脚本里路径会被按 GBK
重新解释成 `E:\AI鐭墽\`，然后 mingw32-make 崩在 `0xC0000409`
（STATUS_STACK_BUFFER_OVERRUN），报错信息完全指不到真正的原因。

这个卷上 8.3 短名生成是关掉的，没有 ASCII 后备路径可用。

**解决办法**：建一个 ASCII 路径的目录联接，从联接进去构建。源码不用动。

```powershell
New-Item -ItemType Junction -Path C:\changji_cpp -Target E:\AI短剧\changji\cpp
```

```bash
cd /c/changji_cpp && cmake -S . -B build -G "MinGW Makefiles" && cmake --build build -j
```

产物 `build/changji.exe` 可以正常拷回任何地方运行，联接只在构建时需要。

### 静态链接

`CHANGJI_STATIC_RUNTIME` 默认 ON。"单一二进制、零运行时依赖"是这个后端
存在的理由之一，不静态链接的话 Windows 上的 exe 要三个 MinGW DLL 才能起来，
拷到别的机器上闪退且没有任何提示。

MinGW 上必须用 `-static` 而不是 `-static-libgcc -static-libstdc++`，
后者漏掉 `libwinpthread-1.dll`。

Linux 上只静态链接 GCC 运行时，不做全静态——glibc 静态链接会让
`getaddrinfo` 出问题，而可选的 ComfyUI 后端要靠域名解析连过去。

### 树莓派

> ⚠️ **尚未验证。** 本机没有 aarch64 交叉编译器，WSL 里也只有
> docker-desktop 发行版。这是阶段 0 唯一未完成的事项，已并入方案第四节的
> 前置验证工程，和另外五项一起在阶段 1 之前做完——等写了两万行才发现
> 某个依赖在 ARM 上编不过，改起来的代价完全不同。
>
> ARM 上还有两件比"能不能编过"更要紧的事，一并在那个工程里验：
> ggml 的 Vulkan 后端在 Pi 5 的 VideoCore VII 上能不能起来，
> 以及 8GB 内存能不能同时装下 Wan 2.2 TI2V-5B 的 Q4 权重、VAE 和系统。
> 后者答案是否，"树莓派跑全流程"这个目标就要重新讨论。

交叉编译（在 WSL 的 Debian/Ubuntu 里）：

```bash
sudo apt install g++-aarch64-linux-gnu
cmake -S . -B build-rpi -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-rpi.cmake
cmake --build build-rpi -j
```

也可以直接在树莓派上本机编译（不用工具链文件）。Pi 5 编 Crow 那几个头
文件很吃内存，8GB 的机器建议先加 swap，或者用 `-j2` 限制并行度。

## 运行

```bash
./build/changji.exe --doctor       # 命令行体检，退出码非 0 表示有必须解决的项
./build/changji.exe --init-config  # 生成带注释的配置模板
./build/changji.exe --port 8080    # 起服务
```

配置优先级：环境变量 > 项目目录的 `changji.toml` > 用户全局配置 > 内置默认值。
用户全局配置的位置与 Python 侧一致（Windows 是 `%LOCALAPPDATA%\changji\config.toml`）。

> **对拍期间两个后端不能共用这一份文件。** 文本推理改成进程内链接
> llama.cpp 之后，`[llm]` 段的字段要变，而 Python 的 `Settings` 是
> `extra="forbid"`，多一个键整份配置就加载失败；`/api/settings` 又是可写
> 接口，两边会互相覆盖。两个后端各用 `--config` 指一份自己的配置。

## 已实现的接口

| 接口 | 说明 |
|---|---|
| `GET /api/health` | `{"ok":true,"service":"changji"}` |
| `GET /api/doctor` | `{can_run, checks:[{name,level,detail,fix}]}` |
| `WS /ws` | 连上收到 `{"type":"hello"}`；上行只认 `subscribe` / `unsubscribe` |

Python 侧共 48 个 REST 接口，这里实现了 2 个。迁移期间两个后端可以同时
跑在不同端口上比对响应。

契约兼容的标准是**字段名、嵌套结构、取值、状态码一致，key 顺序不管**
（早先写的"逐字节兼容"已收回，理由见方案第三节）。四个接口做不到兼容，
在方案的破契约白名单里：`GET /`、`/api/llm/providers`、`/api/llm/models`、
`[llm]` 配置段。

`/api/doctor` 的 `checks` 数组内容不算契约——前端是泛化渲染的，只认
`can_run` 和四个键，检查项本身按各自后端的实际情况写。所以这里叫
"运行时"、"推理服务"而 Python 那边叫 "Python"、"ComfyUI"，是对的。

WebSocket 是加在旁边的第二条通道，不替代任何 REST 接口——前端可以
继续轮询，两种方式并存互为退路。

## 目录

```
src/
├── main.cpp          命令行入口
├── util/
│   ├── paths.*       跨平台标准目录，对齐 Python 的 platformdirs
│   └── proc.*        子进程与 which
├── config/settings.* 配置结构、TOML 读取、环境变量覆盖、校验
├── doctor/doctor.*   环境体检
└── http/
    ├── server.*      Crow 应用与路由
    └── ws.*          WebSocket 连接注册表与广播器
```

## 下一步：前置验证工程

**不是阶段 1。** 阶段 5 定了"直接上 sd.cpp"，没有阶段内退路，所以六项
未知要在写下两万行之前有结论。见方案第四节：

1. MSVC + CUDA 能编（要换工具链）
2. sd.cpp 和 llama.cpp 统一 ggml 后能同时链，各跑一次推理
3. `--offload-to-cpu` 的崩溃 bug（上游 Issue #1483）复不复现
4. `--vae-tiling` 在视频路径上通不通
5. ggml 的 Vulkan 后端在 Pi 5 上能不能起来
6. Pi 的 8GB 装不装得下 5B 的 Q4 权重加 VAE

前四项在 Windows 上做，后两项在 Pi 上做，可以并行。顺带确认 sd.cpp 的
逐步回调和 llama.cpp 的生成有没有中止接口——`/api/stop` 的语义依赖它。

之后才是阶段 1：移植 `models/`（character / project / shot）与项目库读写，
完成标志是能读 Python 写的项目文件（**含中文路径和中文内容**）且字段
全部对得上。
