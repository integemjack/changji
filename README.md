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

## 目录

```
changji/          # 程序本体
  cpp/            # C++ 引擎（界面、接口、编排、推理全在这里）
  webapp/         # 前端源码，打包后嵌进二进制
  docs/           # 方案和手册
```

## 关于根目录这堆 ComfyUI/Docker 的东西

`Dockerfile`、`docker-compose.yml`、`start.ps1`、`download_*.ps1`、
`extra-requirements.txt`、`frontend/`、`bench_shot.py`、`smoke_test.py`
这些是**更早那一版的遗留**：
当时是 Docker 里跑一个 ComfyUI，靠手工摆节点出片。

那条路 2026-09-10 拆掉了——出图、出片、配音、大模型现在全在场记这一个
进程里跑，不需要 ComfyUI，也不需要 Docker。这些文件暂时留着只是备查，
跑现在这套一个都用不到。

（这一页在 2026-09-11 之前一直还是那版旧文档，照着做会去装 ComfyUI。）
