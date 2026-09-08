# changji C++ 后端

场记引擎的 C++ 实现。目标是**一个二进制**：接口、编排、推理全在进程内，
不依赖 Python 解释器。方案见 [../docs/C++重构方案.md](../docs/C++重构方案.md)。

迁移期间它和 Python 引擎并存，靠**对拍**（`tools/duiping.ps1`）保证两边
行为一致。删掉 Python 是阶段 8 的事，闸门见方案里「阶段 8 的清单」。

## 构建

Windows 上是 **MSVC 2022 Build Tools + Ninja**。先进 MSVC 的环境：

```powershell
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && set' |
  ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] } }
```

> 少了这一步的表现是 `fatal error C1083: 无法打开包括文件: "algorithm"`。
> 那不是代码问题，是 `INCLUDE` 没设。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `CHANGJI_SD` | ON | 链 stable-diffusion.cpp，进程内出图出片 |
| `CHANGJI_SD_CUDA` | OFF | sd.cpp 走 CUDA。要 CUDA Toolkit，而且 MSBuild 查的是**版本化**的 `CUDA_PATH_V13_3` 而不是 `CUDA_PATH`，两个都要在进程环境里 |
| `CHANGJI_LLAMA` | OFF | 链 llama.cpp + mtmd，**进程内配音**（阶段 9）。开着会让干净构建多编一份 llama.cpp 和一份打过补丁的 ggml |
| `CHANGJI_BUILD_TESTS` | ON | 编 `changji_tests.exe` 和对拍工具 |
| `CHANGJI_STATIC_RUNTIME` | ON | 静态链运行时。"零运行时依赖"是这个后端存在的理由之一 |

开 `CHANGJI_LLAMA` 时 ggml 由 llama.cpp 提供，并且**就地打上 leejet 的扩展
补丁**（`patches/apply_to_llamacpp.py`，接在 FetchContent 的 `PATCH_COMMAND`
上，幂等）。sd.cpp 硬依赖那些扩展（FP8 类型、int8 convrot、i8 tensorwise
matmul），调用点没有 `#ifdef` 守卫。来龙去脉见
[verify/RESULTS.md](verify/RESULTS.md) 和 [patches/README.md](patches/README.md)。

两个构建目录并存是常态：`build/`（默认）和 `build_llama/`（`-DCHANGJI_LLAMA=ON`）。

## 运行

```bash
./build/changji.exe --doctor       # 命令行体检，退出码非 0 表示有必须解决的项
./build/changji.exe --init-config  # 生成带注释的配置模板
./build/changji.exe --port 8080    # 起服务
```

进程内配音（要 `CHANGJI_LLAMA=ON` 的二进制）：

```bash
# 先取权重：仓库根目录的 download_tts_gguf.ps1，约 1.34 GB
./build_llama/changji.exe --say "雨夜的天台上，他没有回头。"
./build_llama/changji.exe --say "试一句" --tts-model talker.gguf --tts-decoder tok.gguf
```

`--say` 是阶段 9 的实机判据：**不用起服务、不用建项目、不用 ffmpeg**，
出不出得了声一句话就知道。

配置优先级：环境变量 > 项目目录的 `changji.toml` > 用户全局配置 > 内置默认值。

常用环境变量（全部带 `CHANGJI_` 前缀）：`COMFY_BASE_URL`、`LLM_BASE_URL`、
`LLM_MODEL`、`LLM_API_KEY`、`WORKSPACE`、`FFMPEG_PATH`、`VRAM_GB`，
以及 C++ 独有的 `MODELS_DIR` / `MODELS_ENGINE` / `MODELS_VIDEO` /
`MODELS_TTS` / `MODELS_TTS_DECODER` 等。加新的要**在两个地方各写一遍**
（`env_mapping()` 那张表和 `apply_env()`），漏一处不会报错，
`test_models_config.cpp` 里有用例专门拦这个。

## 测试

```bash
./build/changji_tests.exe                                    # 437 条单元测试
powershell -ExecutionPolicy Bypass -File tools\duiping.ps1   # 对拍（会自己起四个进程）
powershell ... -File tools\duiping.ps1 -CppExe build_llama\changji.exe
powershell ... -File tools\duiping.ps1 -Case 改台词 -ShowRequests
```

对拍一条命令起齐四个进程（Python 后端、C++ 后端、两个假大模型），跑完收干净。
**两个假模型不共用**：`/api/plan` 一次要问模型两遍，共用队列会永远错位一条。

九层覆盖：数据层往返、录制 GET、实时 GET、写接口、编辑接口、上传（multipart）、
`/api/media` 的 Range、LLM 阶段（**逐字节比提示词**）、推理层。
当前 **165 条一致、0 条不同、跳过 0 条**。

「跳过」是零，这是专门去挣的：一条跳过的用例和一条通过的用例在总数里长得
一样，但前者什么都没保证。三处有意的偏差（`GET /`、多区间 Range、
Python 的 StoryboardError 穿透 bug）都是**验**而不是跳过——既验 Python 还是
那个样子，也验 C++ 确实回了想要的。

## 目录

```
src/
├── main.cpp        命令行入口（--doctor / --init-config / --say / 起服务）
├── util/           路径、子进程、文本、时间、东亚字宽
├── config/         配置结构、TOML 读写、环境变量覆盖、校验
├── models/         project / character / shot 与项目库读写
├── http/           Crow 路由 + 各接口（只读、编辑、上传、媒体、LLM、run）
├── llm/            OpenAI 兼容客户端
├── stages/         剧本、圣经、分镜、提示词、首帧、渲染、配音
├── infer/          sd.cpp 门面、显存调度、ggml ABI 探针、进程内配音
├── comfy/          ComfyUI 可选后端（工作流转换、客户端、WebSocket）
├── media/          ffmpeg、字幕、装配
├── gates/          质量闸门
├── pipeline/       job 表与整集流水线
└── doctor/         环境体检

tests/
├── unit/           单元测试，读 tests/golden/ 里的语料
├── compat/         对拍工具（changji_compat.exe）
├── golden/         28 份金语料，由 Python 侧导出，**进版本库**
└── export_*.py     语料导出脚本（import Python 引擎）

tools/              对拍与语料生成的脚手架
patches/            leejet/ggml 扩展补丁集 + 自动应用脚本
verify/             前置验证工程（一次性，结论在 RESULTS.md）
```

## 现在到哪儿了

阶段 0–7 代码完成，阶段 9 的链路也通了（编译、链接、自检），
**但推理那部分一次都没真跑过**——这台机器上没有模型、没有 ffmpeg。

| 还差 | 需要 |
|---|---|
| 配音真出声 | Qwen3-TTS 权重（`download_tts_gguf.ps1`，1.34 GB），或者起一个 ComfyUI |
| 首帧 / 视频 / 成片 | Wan 2.2 权重（约 11.3 GB）+ ffmpeg |
| 阶段 8 删 Python | 以上跑通之后。**那是单向门**，见方案里「阶段 8 的清单」 |

契约兼容的标准是**字段名、嵌套结构、取值、状态码一致，key 顺序不管**；
**提示词的拼接要逐字节一致**；校验错误的**文字**不算契约。
破契约项重审之后只剩 `GET /` 一条，不影响前端。
