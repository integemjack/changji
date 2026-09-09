# 一台新 GPU 机器上把 changji 跑起来

8×L20 那台上踩完一遍之后整理的。**每一条都写了为什么**——不写的话下次
换机器又会照着抄一遍错的。

## 0. 先看显存够不够

一进程一张卡，**不做跨卡切分**，所以只看单卡容量：

| 阶段 | 要常驻的 | 合计 |
|---|---|---|
| 出片 | Wan 5B fp16 9.4 GB + umt5-xxl 11 GB + VAE 1.4 GB | **≈22 GB** |
| 出首帧 | Qwen-Image fp8 20 GB + Qwen2.5-VL **bf16** 16 GB | **≈36 GB** |
| 出首帧（省显存） | Qwen-Image fp8 20 GB + Qwen2.5-VL **Q8_0 GGUF** ≈8 GB | **≈28 GB** |

- **≥40 GB**（L20 48 GB / A100）：随便配，bf16 编码器也行。
- **32 GB**（5090）：文本编码器要用 **`Qwen2.5-VL-7B-Instruct-Q8_0.gguf`**
  （约 8 GB，sd.cpp 上游 `docs/qwen_image.md` 里的命令行用的就是它）。
  用 bf16 那份（16 GB）会超，auto_fit 只能把一部分赶回内存，出首帧那一步变慢。
  **不要用 Comfy 的 `fp8_scaled`**——见下面第 1 节第 2 条。
- **≤24 GB**：`weights = "cpu"`，权重放系统内存、用到才搬进显存。能跑，
  但每张卡大约 1 秒忙 2 秒闲（实测 35% 利用率），瓶颈在 PCIe。

## 1. 模型

放一个目录（下面按 `/data/models`），`[models].dir` 指过去。

    # 出片：Wan 2.2 TI2V-5B。注意 VAE 是 2.2 的，不是 2.1 的（只有 5B 用它）
    wan2.2_ti2v_5B_fp16.safetensors          9.4 GB
    Wan2.2_VAE.safetensors                   1.4 GB
    umt5_xxl_fp16.safetensors                 11 GB

    # 出首帧：Qwen-Image **基础模型**
    qwen_image_fp8_e4m3fn.safetensors         20 GB
    qwen_image_vae.safetensors               243 MB
    qwen_2.5_vl_7b_bf16.safetensors           16 GB   # ≥40 GB 卡用这份
    Qwen2.5-VL-7B-Instruct-Q8_0.gguf         ~8 GB   # 32 GB 卡用这份

    # 配音：Qwen3-TTS
    Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf       3.3 GB
    mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf 639 MB

**两个坑，都真栽过：**

1. **别用 `qwen_image_edit_*`。** 那是图生图**编辑**模型，上游文档每条命令都带
   `-r 参考图`。拿它做文生图出来的是**彩色雪花**，而且方差比真图还大——
   靠"方差不为零"判"不是空图"会一路绿灯。抽帧看一眼，十秒钟的事。
2. **文本编码器别用 Comfy 的 `fp8_scaled` 那份。** 那种格式带 scale 张量，
   sd.cpp 的加载器里没有一行处理 scaled，会按普通 fp8 读——不报错，
   只是文本条件全是垃圾。用 `bf16`（大卡）或 `Q8_0` 的 GGUF（小卡），
   后者正是上游 `docs/qwen_image.md` 命令行里用的那份。

国内拉权重：`aria2c -x 8 -s 8 -k 4M -o <名字> https://hf-mirror.com/...`。
单连接只有 1～2 MB/s，八连接能跑满带宽。

## 2. 配置

`~/.config/changji/config.toml`（工作进程的 systemd 单元里要设
`Environment=HOME=/root` 和 `XDG_CONFIG_HOME=/root/.config`，
**systemd 不会自己给 HOME**，缺了它工作进程读的是内置默认值然后报"没配模型"）。

    # 只影响档位表（分辨率/步数），不影响真正装了什么。
    # Wan 2.2 TI2V-5B 的训练分辨率是 1280×704，成片档超出去只会更慢不会更好，
    # 所以哪怕卡有 48 GB 也填 20 —— 20 落在 ≥15.5 那一档：
    #   草稿 640×352/10 步，预览 960×544/20 步，成片 1280×704/30 步
    vram_gb_override = 20

    [models]
    engine = "sd"
    dir = "/data/models"
    # 权重放哪。auto = 交给 sd.cpp 按这张卡真实的空闲显存决定，装得下就常驻；
    # cpu = 全放系统内存（小卡唯一的选择，慢）
    weights = "auto"
    video = "wan2.2_ti2v_5B_fp16.safetensors"
    video_vae = "Wan2.2_VAE.safetensors"
    video_text_encoder = "umt5_xxl_fp16.safetensors"
    image = "qwen_image_fp8_e4m3fn.safetensors"
    image_vae = "qwen_image_vae.safetensors"
    image_text_encoder = "qwen_2.5_vl_7b_bf16.safetensors"
    tts = "Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf"
    tts_decoder = "mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf"
    # 首帧按哪个档位出。默认 draft（和 Python 一样），但首帧是跨镜头一致性的
    # 锚点、又会当起始图喂给出片那一步——草稿档 352×640 的首帧配成片档
    # 704×1280 的视频等于把锚点放大两倍再用。显存够就填 final。
    frame_tier = "final"

    [tts]
    backend = "local"          # 进程内 Qwen3-TTS，不用起别的服务

    [workers]
    gpu = 7                    # 协调者自己（进程内配音）绑哪张卡
    endpoints = ["http://127.0.0.1:9001", ...]   # 一张卡一个

    [llm]
    base_url = "http://127.0.0.1:8081/v1"
    model = "qwen3-14b"
    api_key = "none"
    timeout_s = 1800.0

## 3. 进程

一张卡一个工作进程，协调者一个，大模型一个。

    # 工作进程（systemd 模板，%i 是卡号）
    Environment=CUDA_VISIBLE_DEVICES=%i
    Environment=HOME=/root
    Environment=XDG_CONFIG_HOME=/root/.config
    ExecStart=/root/changji/build-worker/changji --worker --gpu %i --port 900$((%i+1))

    # 协调者。--host 0.0.0.0 才能从外面开 webapp（端口就是这个 --port）。
    # **这套接口没有鉴权**，连上就能读项目、改分镜、起流水线。
    ExecStart=/root/changji/build-coord/changji --port 8080 --host 0.0.0.0
    Environment=CHANGJI_WORKSPACE=/root/.local/share/changji/projects

    # 大模型：找一张没被工作进程占的卡
    CUDA_VISIBLE_DEVICES=6 llama-server -m Qwen3-14B-Q4_K_M.gguf \
      --port 8081 --host 127.0.0.1 -ngl 99 -c 65536 \
      --chat-template-kwargs '{"enable_thinking":false}'

**别加 `--reasoning-format none`**：加了它空的 `<think></think>` 会原样留在
content 里；默认（auto）会剥到 `reasoning_content`，content 才干净。

## 4. 两份构建

    bash cpp/tools/build_gpu_box.sh /root/changji /root/autodl-tmp 120

架构号：Ada（L20/4090）= 89，Blackwell（5080/5090）= **120**。填错编出来的
东西在卡上跑不了，而且要跑到加载模型那一步才报错。

脚本先编一份**纯 CPU 的测试目标**（最快能发现编译器问题：新机器的 g++ 可能
比你上次用的老，C++20 有些地方不认），再编工作进程（sd.cpp + CUDA）和
协调者（llama.cpp + CUDA，进程内配音）。

**构建目录别放系统盘。** AutoDL 这类机器 `/` 只有 30 G，而 FetchContent 拉的
那堆（llama.cpp + sd.cpp + ggml + Crow + asio…）加上 CUDA 目标文件轻松几个 G，
编到一半没空间比编不过还难查。

**国内机器上 GitHub 可能直连不通。** `git ls-remote https://github.com/...`
会挂着不返回，FetchContent 就卡在克隆那一步，**日志里一个字都不打**——
看起来像"编译很慢"，实际是永远不会好。AutoDL 自带 `/etc/network_turbo`，
`source` 一下就通（脚本会自动找它）。**它会让 pip / apt 变慢**，
所以只在构建时开；下模型别开，hf-mirror 本来就是国内的。


## 5. 跑之前先看一眼画面

**先看画面，再量速度。** 在 L20 那台上先量了 39 分钟的速度，才发现量的是雪花。

    # 只出首帧，抽一张出来看
    curl -X POST :8080/api/run -d '{"project":"...","episode_id":"ep01",
                                    "stages":["frames"],"force":true}'
    # 然后真的把 frames/*.png 打开看

分不出来的时候用上游的 `sd-cli` 拿同一批权重跑一遍，能把"我们的代码"
和"模型/旋钮"分开。

## 6. 已知的坑

- **`pkill -f "..."` 在 ssh 里会杀掉自己**（模式匹配到 `bash -c` 那行命令行）。
  用 `pkill -x` 或锚定 `^`。
- **后台起进程要 `setsid nohup … </dev/null >log 2>&1 &`**，否则 ssh 会挂在
  pty 上直到超时；掉线还会把部署打断到一半。
- **`tar` 同步源码带的是本机 mtime**，两边时钟不一致时 cmake 会认为目标文件
  比源码新、**静默不重编**。解压后 `touch` 改过的文件再编。
- **正在跑的二进制不能原地重编**（链接时 ETXTBSY），先停服务或另起构建目录。
- **显存占用不能当"这张卡在忙"的证据**——工作进程跑完不卸模型。看 utilization。
- 云厂商的安全组要放行 webapp 那个端口，机器里绑了 0.0.0.0 不等于外面能连。
