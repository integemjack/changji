# changji C++ 后端

方案见 [../docs/C++重构方案.md](../docs/C++重构方案.md)。

当前进度：**阶段 0（骨架）**。只有 `/api/health`、`/api/doctor` 和一个
WebSocket 端点，目的是把工具链和跨平台构建先跑通。

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
`getaddrinfo` 出问题，而我们要连 LLM 和推理服务，全是域名解析。

### 树莓派

> ⚠️ **尚未验证。** 阶段 0 的验收要求包含这一项，但本机没有 aarch64
> 交叉编译器，WSL 里也只有 docker-desktop 发行版。这是阶段 0 唯一
> 未完成的事项，动手写阶段 1 之前应该先补上——等写了两万行才发现
> 某个依赖在 ARM 上编不过，改起来的代价完全不同。

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
用户全局配置的位置与 Python 侧一致（Windows 是 `%LOCALAPPDATA%\changji\config.toml`），
两个后端读的是同一份文件。

## 已实现的接口

| 接口 | 说明 |
|---|---|
| `GET /api/health` | `{"ok":true,"service":"changji"}` |
| `GET /api/doctor` | `{can_run, checks:[{name,level,detail,fix}]}` |
| `WS /ws` | 连上收到 `{"type":"hello"}`；上行只认 `subscribe` / `unsubscribe` |

Python 侧共 48 个 REST 接口，这里实现了 2 个。**契约必须逐字节兼容**，
迁移期间两个后端可以同时跑在不同端口上比对响应。

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

## 下一步（阶段 1）

移植 `models/`（character / project / shot）与项目库读写。
完成标志是能读 Python 写的项目文件且字段全部对得上。
