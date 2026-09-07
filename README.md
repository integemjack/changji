# AI 短剧 — ComfyUI (Docker) + Wan 2.2

本机环境：RTX 5080 16GB / Ryzen 9 9950X / 31GB 内存 / Windows 11

## 目录结构

```
E:\AI短剧\
├── Dockerfile              # PyTorch 2.14 + CUDA 13.0（50 系显卡必须 cu130+）
├── docker-compose.yml      # 服务定义，GPU 直通，整个 ComfyUI 目录挂载到容器
├── extra-requirements.txt  # 自定义节点的额外 pip 依赖，改完要重新 build
├── .dockerignore           # 防止把模型文件塞进构建上下文
├── start.ps1               # 一键启动
├── download_models.ps1     # 模型下载（断点续传+自动重试）
└── ComfyUI\                # 代码、模型、输出都在这里，容器重建不丢
    ├── models\
    │   ├── diffusion_models\wan2.2_ti2v_5B_fp16.safetensors
    │   ├── vae\wan2.2_vae.safetensors
    │   └── text_encoders\umt5_xxl_fp8_e4m3fn_scaled.safetensors
    ├── output\             # 生成的视频在这里
    ├── input\              # 首帧图片放这里
    └── user\default\workflows\Wan2.2_5B_图生视频.json
```

## 使用

启动：

```powershell
pwsh -File E:\AI短剧\start.ps1
```

然后浏览器打开 http://localhost:8188

常用命令：

```powershell
docker logs -f comfyui                                    # 看日志
docker compose -f E:\AI短剧\docker-compose.yml down       # 停止
docker compose -f E:\AI短剧\docker-compose.yml build      # 改了依赖后重建
docker compose -f E:\AI短剧\docker-compose.yml up -d      # 启动
```

## 跑第一个镜头

1. 把首帧图片放进 `ComfyUI\input\`
2. 界面左上角 Workflow → Open，选 `Wan2.2_5B_图生视频`
3. 在 LoadImage 节点选你的图片
4. 改正向提示词，描述这个镜头要发生什么
5. 点 Run，视频输出到 `ComfyUI\output\video\`

## 参数说明

| 节点 | 参数 | 说明 |
|---|---|---|
| Wan22ImageToVideoLatent | 1280x704 | 分辨率，显存紧张就降到 960x544 |
| Wan22ImageToVideoLatent | length 121 | 帧数，121 帧 @24fps = 5 秒 |
| KSampler | steps 30 | 步数，20 出草稿，30-40 出成片 |
| KSampler | cfg 5 | 提示词遵循度，3-7 之间调 |
| ModelSamplingSD3 | shift 8 | 运动幅度，值大动作大 |
| CreateVideo | fps 24 | 帧率，要和帧数换算对应 |

## 装自定义节点

界面里用 Manager 装（已内置）。如果某个节点需要额外的 pip 包，写进
`extra-requirements.txt` 然后重新 build，否则容器重建后依赖会丢。

## 后续要加的环节

- 配音：Qwen3-TTS（中文和方言强，4GB 显存够用）
- 口型：LatentSync 1.6，或用 Wan 的 InfiniteTalk 分支直接音频驱动
- 角色一致性：主角训 LoRA + IPAdapter 锁脸 + ControlNet 锁姿势
- 剪辑：FFmpeg 脚本批量拼接，或导入剪映精修

## 已知问题处理

Docker Desktop 报 `initializing Inference manager` 或 `Secrets Engine` 启动失败，
是残留的 socket 文件删不掉。处理方法：关掉 Docker，把
`%LOCALAPPDATA%\Docker\run` 和 `%LOCALAPPDATA%\docker-secrets-engine`
两个目录改名，再重新创建空目录，然后启动。本机已把 Docker AI 关掉减少复发。
