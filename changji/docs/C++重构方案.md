# C++ 后端重构方案

制定时间 2026-09-07。

## 目标

把现在的三层（Python 引擎 + ComfyUI + Node 界面）收敛成两层：

- **C++ 单一二进制**：编排 + 推理 + HTTP 服务
- **Node**：只做界面

产出物是一个 exe，零运行时依赖，Windows 和树莓派都能直接跑。

约束顺序：**能跑 > 画质 > 速度**。低配硬件上时间不是问题，显存才是。

## 为什么是 C++

不是为了快。编排层是纯 IO 胶水，语言对性能没有影响。

真正的理由只有两个：

1. **推理层必须是 C++**。sd.cpp 和 llama.cpp 都基于 ggml，链接进来才能拿到
   逐层权重换入换出的控制权，让 16GB 的模型在 6GB 卡上跑起来。这是整个
   项目的立项理由
2. **单一二进制**。低配设备上装 Python 环境和 Node 运行时本身就是负担

代价要认：编排层用 C++ 大约要 2.5 倍行数，且没有 pydantic 的等价物。
这是为上面两条付的学费，不是 C++ 在这一层更优。

---

## 一、工作量盘点

现有 Python 引擎 11,169 行。其中：

| 处理方式 | 模块 | 行数 |
|---|---|---|
| **直接删** | `comfy/`（客户端 + 工作流格式转换） | 604 |
| **直接删** | `web/page.py`（旧单页界面，Node 接管） | 1,563 |
| **直接删** | `workflows_loader.py` | 57 |
| **暂缓** | `cli.py`（命令行入口，后期再补） | 366 |
| **需要移植** | 其余全部 | 8,579 |

`comfy/workflow.py` 那 222 行是纯粹为了对付 ComfyUI 的
界面版/接口版格式转换（`widgets_values` 错位那一坨），
直接链接 sd.cpp 之后这个问题从根上消失。

移植目标约 8,579 行 Python，预计对应 **18,000 ~ 22,000 行 C++**。

## 二、技术选型

| 用途 | 选型 | 理由 |
|---|---|---|
| HTTP + WebSocket 服务端 | **Crow** | 路由写法接近 Flask/FastAPI，48 个接口能近乎一一对译；WebSocket 内置；header-only，交叉编译到 ARM 省事 |
| HTTP 客户端 | **cpp-httplib** | 调 LLM 和 TTS 侧车用。单头文件，只用客户端那半边 |
| JSON | **nlohmann/json** | 无争议。`NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT` 能覆盖序列化和默认值 |
| 视频/图像推理 | **stable-diffusion.cpp** | 已支持 Wan 2.2 TI2V-5B，含 `--offload-to-cpu` 逐层卸载、`--vae-tiling`、`--diffusion-fa` |
| 文本推理 | **llama.cpp** | 替代 Ollama，剧本/圣经/分镜三个阶段用 |
| 音视频 | **ffmpeg 子进程** | 保持现状。链接 libav 收益不足以抵消复杂度 |
| 配音 | **Python 侧车服务** | 见风险一节 |
| 构建 | **CMake + FetchContent** | 交叉编译树莓派用 toolchain file |

### 不需要的东西

- **业务逻辑的异步化**。Crow 自带 asio 事件循环处理连接，但六个阶段的
  代码保持同步写法，跑在独立的工作线程上。Python 那边满屏的 `async` 是
  FastAPI 的框架要求，不是业务需求，不要照搬
- **ORM / 数据库**。项目数据是文件系统上的 JSON，保持原样

### pydantic 的替代

C++ 没有等价物，这是移植中最枯燥的一块。采用的模式：

```cpp
struct Shot {
    std::string shot_id;
    int duration_ms = 3000;
    // ... 字段
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Shot, shot_id, duration_ms, ...)

    // 校验单独一个方法，返回错误列表而不是抛异常。
    // Python 那边 pydantic 的 field_validator 逐条翻译到这里。
    std::vector<std::string> validate() const;
};
```

约束：**校验逻辑必须和 Python 的 validator 一一对应**，对拍时才能发现漏移植。

---

## 三、架构

```
┌─────────────────────────────────┐
│ Node (webapp)                   │   只做界面
│   Vue 客户端 + Express 静态服务  │   engine/ 目录整个删掉
└──────────────┬──────────────────┘
               │ HTTP，接口契约不变
┌──────────────▼──────────────────┐
│ changji (单一 C++ 二进制)        │
│  ┌───────────────────────────┐  │
│  │ http/     48 个接口        │  │
│  │ pipeline/ 编排 + 任务状态  │  │
│  │ stages/   六个阶段          │  │
│  │ models/   数据结构 + 校验   │  │
│  └───────────────────────────┘  │
│  ┌───────────────────────────┐  │
│  │ 链接: sd.cpp / llama.cpp   │  │
│  │ 子进程: ffmpeg             │  │
│  └───────────────────────────┘  │
└──────────────┬──────────────────┘
               │ HTTP（唯一的外部依赖）
        ┌──────▼──────┐
        │ TTS 侧车     │  Python，见风险一节
        └─────────────┘
```

### 核心策略：接口契约不变

**C++ 后端必须与现有 Python 引擎的 48 个接口逐字节兼容。**

这一条是整个迁移能否安全推进的关键：

- Node 前端零改动
- 改一个地址就能在 Python 后端和 C++ 后端之间切换
- 任何时刻两个后端都能同时跑，同样的请求打过去比对响应
- 出问题随时切回去

不允许借重构之机顺手改接口。要改接口，等 Python 删掉之后再说。

### WebSocket：增量，不是替换

**48 个 REST 接口原样保留，WebSocket 是加在旁边的第二条通道。**

这样契约兼容的策略才不会破：Node 前端可以先不动（继续轮询），
等 C++ 后端稳定后再改成订阅 WebSocket。两种方式并存，互为退路。

推送的理由是现有代码里已经论证过的——[comfy/client.py](../src/changji/comfy/client.py)
开头写着：*"用 WebSocket 监听进度而不是轮询历史接口，因为成片档一个镜头
要跑好几分钟，轮询既慢又容易漏掉中间状态。"* sd.cpp 的采样循环有逐步回调，
能推「第 12/30 步」这种真实时进度，轮询拿不到。

#### 消息格式

服务端下行，一律 JSON，带 `type` 和 `job_id`：

```json
{"type": "progress", "job_id": "...", "stage": "render",
 "shot_id": "S03", "step": 12, "total": 30}
{"type": "stage",    "job_id": "...", "stage": "audio", "status": "started"}
{"type": "log",      "job_id": "...", "level": "warn", "text": "..."}
{"type": "done",     "job_id": "...", "outputs": [...]}
{"type": "error",    "job_id": "...", "message": "..."}
```

客户端上行只有订阅控制：`{"type":"subscribe","job_id":"..."}` 和 `unsubscribe`。
**不通过 WebSocket 发业务指令**，那些走 REST。上行通道越窄越好。

#### 两条从 ComfyUI 那边学到的教训，必须带过来

1. **订阅按 job_id 记，不按连接记。** ComfyUI 按 clientId 记订阅，
   同一个 id 上并发跑两个任务，后连的会把先连的挤下线，先连的永远
   等不到完成消息。新设计里一个连接可订阅多个 job，一个 job 可被多个连接订阅，
   两者是多对多，不是一对一

2. **WebSocket 断线不是致命错误。** 服务端必须把每个 job 的最新状态
   留在内存里，REST 的 `GET /api/run` 仍然能查到完整状态。前端 WS 掉线
   就退回轮询，重连后先拉一次全量状态再续订阅。任务本身不受连接影响

#### 线程模型

```
Crow 事件循环线程     ──  处理 HTTP 请求 + WebSocket 收发
工作线程（1 个）      ──  跑流水线，同步写法
共享状态              ──  互斥量保护的 job 表 + 连接注册表
```

工作线程产生进度 → 写进 job 表 → 通知广播器 → 广播器在事件循环线程上
向订阅了该 job 的连接发送。

需要小心的两处：

- **连接生命周期**。广播时连接可能已经关闭。连接注册表的增删和遍历
  必须在同一把锁下，或者用 Crow 提供的连接句柄有效性检查
- **推送频率**。逐步回调每秒可能触发几十次，直接透传会把前端淹掉。
  加节流，最多每 200ms 推一次，并且**完成消息不受节流影响**

### 目录结构

```
cpp/
├── CMakeLists.txt
├── toolchains/aarch64-rpi.cmake
├── src/
│   ├── main.cpp
│   ├── http/          # 接口层，按 Python 的 server.py 分组
│   ├── pipeline/      # 编排、任务状态机、工作线程
│   ├── stages/        # bible / script / storyboard / frames / render / audio
│   ├── models/        # character / project / shot + 校验
│   ├── infer/         # sd.cpp 和 llama.cpp 的封装
│   ├── assembly/      # ffmpeg 子进程、字幕、成片组装
│   └── util/          # 配置、日志、文件系统
├── tests/
│   └── golden/        # 对拍用的黄金文件
└── third_party/
```

---

## 四、迁移顺序

原则：**每个阶段结束时系统都是可运行的**，不存在"写完才能跑"的中间态。

| 阶段 | 内容 | 完成标志 |
|---|---|---|
| **0** | CMake 骨架、Crow 起 HTTP、配置读写、`/api/health` `/api/doctor`；WebSocket 打通一个 echo 端点 | Windows 和树莓派上都能编出二进制、响应健康检查、WS 能连上 |
| **1** | `models/` 三个数据结构 + 项目库读写 | 能读 Python 写的项目文件，字段全部对得上 |
| **2** | 只读接口：`/api/project` `/api/shots` `/api/assets` `/api/settings` `/api/hardware` | Node 前端指向 C++ 后端，能正常显示项目 |
| **3** | 编辑接口：`/api/shot` `/api/character` `/api/style` `/api/location` 等 | `test_web_editing.py` 的 1083 行全部对拍通过 |
| **4** | LLM 三阶段：bible / script / storyboard，接 llama.cpp；工作线程 + job 表 + WebSocket 进度广播 | 用录制的 LLM 响应，生成结果与 Python 一致；WS 能实时看到阶段推进 |
| **5** | 推理接入：链接 sd.cpp，frames + render；逐步进度接进 WS 广播 | 能出图能出视频，与 ComfyUI 输出做画质比对；前端能看到「第 12/30 步」 |
| **6** | 组装：ffmpeg、字幕、成片 | 能产出完整一集 |
| **7** | TTS 侧车对接 | 配音链路通 |
| **8** | 删 Python 引擎、删 Node 的 `engine/` 目录 | 只剩两层 |

阶段 0 必须包含树莓派交叉编译，不要等到最后才发现某个依赖在 ARM 上编不过。

---

## 五、对拍验证

Python 引擎保留的唯一目的：**当回归基准**。删除之前必须完成对拍。

### 数据层对拍（阶段 1-3）

Python 和 C++ 读写同一批项目文件，逐字段比对 JSON。
差异必须为零——包括字段顺序无关的深比较、浮点数精度、空值处理。

### 接口层对拍（阶段 2-4）

两个后端同时启动在不同端口，同一组请求打过去比对响应体。
现有 5,781 行 pytest 是请求语料的来源，把它们的输入抽出来复用。

### LLM 阶段对拍（阶段 4）

LLM 输出不确定，不能直接对拍。做法：

1. 用 Python 跑一遍，把每次 LLM 的请求和响应录成 JSON 文件
2. C++ 侧接一个"回放模式"，读录制文件而不真的发请求
3. 比对提示词拼接结果（必须逐字节相同）和后续解析结果

提示词拼接尤其要严：[render.py](../src/changji/stages/render.py) 里的分层顺序
（身份层 → 场景层 → 镜头层 → 风格层）不能变，顺序一改画面重心就变。

### 推理层对拍（阶段 5）

不能要求逐像素相同——不同的实现、不同的量化格式必然有数值差异。
做法是固定种子生成同一个镜头，人工比对画质，重点看：

- 角色长相是否一致（这是整个项目的核心指标）
- 有无量化导致的色带、糊面、细节丢失
- 分块 VAE 解码有无可见接缝

---

## 六、风险

### 风险一：TTS 没有 C++ 出路（高，已确认）

现有 `models/TTS/` 里 17GB 权重是 CosyVoice、Qwen3-TTS、higgs_audio_v3，
全部是 Python 模型，没有 C++ 实现。

sherpa-onnx 是可用的 C++ TTS 运行时，但只覆盖 VITS/Kokoro 那一档，
中文表现明显低于 CosyVoice。而 [选型结论.md](选型结论.md) 在 TTS 的
许可证和中文质量上是筛过的，降级会丢掉那部分工作成果。

**决定：配音保留一个 Python 侧车服务。**"单一二进制"这个目标在配音这一环
打个折扣。现有 [audio.py](../src/changji/stages/audio.py) 的 `HttpTTSBackend`
本来就是为这种情况留的口子，C++ 侧照着它的协议实现客户端即可。

如果一定要纯二进制，代价是接受 sherpa-onnx 的中文质量。这个取舍留给后面决定，
不阻塞前面七个阶段。

### 风险二：ggml 版本冲突（中）

sd.cpp 和 llama.cpp 各自 vendor 了不同 commit 的 ggml。同时链接进一个
二进制会有符号冲突或 ABI 不兼容。

缓解：阶段 5 之前先做一个最小验证工程，只做"两个库同时链接并各跑一次推理"。
如果冲突无法调和，退路是 llama.cpp 走子进程，只链接 sd.cpp。

### 风险三：Wan 视频 VAE 的分块解码可能缺失（中）

上游文档明确写了 Wan 的 VAE 吃显存极多，兜底手段是换 TAE（质量下降）。
`--vae-tiling` 在图像端确认可用，视频路径上是否接通未确认。

这是低显存目标的关键一环。如果确实缺失，需要自己实现并回馈上游。
分块必须带重叠区和羽化混合，否则出现可见接缝网格。

### 风险四：`--offload-to-cpu` 有崩溃 bug（中）

上游 [Issue #1483](https://github.com/leejet/stable-diffusion.cpp/issues/1483)
报告该选项会导致进程终止。这是本方案最依赖的功能，阶段 5 开始前先验证是否复现。

### 风险五：业务知识随 Python 删除而流失（中）

`stages/` 里积累的不只是代码：提示词分层顺序及其理由、画质档位的实测标定值、
各种边界情况的处理。这些以注释形式存在于 Python 源码里。

缓解：移植时**注释一并翻译过去**，不要只译代码。删除 Python 之前
通读一遍 `stages/` 和 `hardware.py` 的注释，确认没有遗漏的隐性约束。

---

## 七、暂不处理

- `cli.py` 366 行命令行入口，等 HTTP 层稳定后再补
- 树莓派上的 ffmpeg 软件编码性能（Pi 5 无硬件 H.264 编码器），
  先用 `-c copy` 拼接规避，需要重编码时再评估
- 接口设计的改进，一律等 Python 删除之后再谈
