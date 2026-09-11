# 场记 changji

AI 短剧生产流水线：从剧本到成片的本地编排引擎。

**一个二进制跑全部**——界面、接口、编排、出图、出片、配音、大模型全在
一个进程里，没有第二个服务要起。

👉 **完整文档在 [changji/README.md](changji/README.md)**（这是什么、设计要点、
安装、Web 平台、配音、已知问题）。

## 装

```bash
curl -fsSL https://raw.githubusercontent.com/integemjack/changji/main/changji/install.sh | bash
```

或去 [Releases](https://github.com/integemjack/changji/releases) 下对应平台的包。
Windows / macOS / Linux，x64 和 arm64 都有。装完先跑一遍体检：

```bash
changji --doctor        # 缺什么它会说，并且给出怎么办
changji --port 8080     # 起服务，浏览器打开 http://127.0.0.1:8080
```

## 根目录长什么样

```
changji/                # 程序本体（cpp 引擎 + webapp 源码 + docs）
download_wan_gguf.ps1   # 拉出片模型的 GGUF，给进程内的 sd.cpp
download_tts_gguf.ps1   # 拉配音模型的 GGUF，给进程内的 llama.cpp + mtmd
docs/                   # 选型结论
models/  bin/           # 本机的权重和外部可执行文件，不进版本库
```

ComfyUI + Docker 那一版的东西 **2026-09-11 全删了**：`Dockerfile`、
`docker-compose.yml`、`.dockerignore`、`start.ps1`、`extra-requirements.txt`、
`frontend/`（ComfyUI 自带前端，570 个文件 31 MB）、
`download_models.ps1`、`download_tts_model.ps1`（往 `ComfyUI\models` 下
safetensors 的那两份）、`bench_shot.py`、`smoke_test.py`（连 :8188 跑
workflow 的那两个）。

出图、出片、配音、大模型现在全在场记这一个进程里跑，不需要 ComfyUI，
也不需要 Docker。要翻旧账的话 git 历史里还在。

**留下的那两个 `download_*_gguf.ps1` 不是漏网的**：它们下的是 GGUF，
给现在这套用的，和上面删掉的 safetensors 那两份不是一回事。
