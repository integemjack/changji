# 场记 changji

AI 短剧生产流水线。从剧本到成片的本地编排引擎，支持真人与动漫双风格。

名字取自影视工作里的「场记」，这个岗位负责的正是这套软件要做的事：
维护分镜表、盯住跨镜头的连续性、记录每一条的状态。

## 这是什么

它不是生成模型，是把一堆生成模型串成生产线的编排层。
**一个二进制**：界面、接口、编排、出图、出片、配音、大模型全在进程内，
没有第二个服务要起。大模型也可以指到远端任何兼容 OpenAI 接口的服务上。
场记负责的是分镜表、资产库、任务调度、质量闸门和成片装配。

## 设计要点

**分镜表是中枢。** 所有阶段读写同一份结构化数据，阶段之间不传自由文本。

**角色一致性做成结构约束。** 分镜表的 schema 里没有任何描述长相、发型、服装的
字段，大模型只能填角色 id 和本镜可变项。外观描述由程序从资产库机械拼接，
逐字节相同。这比在提示词里强调「保持一致」有效得多。

**配音先行。** 先跑配音拿到真实时长，再反推锁定镜头时长。音画从源头对齐。

**分级生成加质量闸门。** 草稿档比成片档快十几倍，叙事和构图的判断在草稿档做完，
只有通过闸门的镜头才升级到成片档。这是无人值守量产的可行性来源。

## 可移植性

- 安装目录和项目数据目录分开，换机器把项目目录拷走即可
- 大模型和配音都能改成远端地址，指向局域网里任意一台有显卡的机器
- 画质档位由运行时探测到的显存推导，不写死
- 配置优先级：环境变量 > 项目配置 > 用户全局配置 > 默认值

## 安装

下一个二进制就能用，没有运行时依赖（除了 ffmpeg，装配和字幕烧录要它）。

```bash
curl -fsSL https://raw.githubusercontent.com/Ireoo/changji/main/changji/install.sh | bash
```

或者去 [Releases](https://github.com/Ireoo/changji/releases) 直接下对应平台的包。
Windows / macOS / Linux，x64 和 arm64 都有，包名形如
`changji-linux-arm64.tar.gz`。

自己编见 [cpp/README.md](cpp/README.md)。

```bash
changji --doctor        # 体检，缺什么它会说
changji --init-config   # 生成一份带注释的配置模板
changji --port 8080     # 起服务
```

## Web 平台

引导式界面在 `webapp/`，Node + Vue 3 写的，把整条生产线分成八步：

```
全剧（做一次）    项目 → 剧本大纲 → 角色
分集（每集重复）  场景 → 分镜 → 制作 → 成片 → 上传至平台
```

每一步都能让大模型代劳，每一步的结果都看得见。环境和参数全收在设置页，
包括大模型地址和各个模型权重的位置。电脑和手机都能开。

**打包好的前端嵌在二进制里**，起了服务直接打开
http://127.0.0.1:8080 就是它，不用装 Node：

```bash
changji --port 8080
```

改前端的时候才需要那一套开发服务器：

```bash
cd webapp && npm install && npm run dev
```

或者 `docker compose up -d` 一次拉起全套，然后打开 http://localhost:5174 。
细节见 `webapp/README.md`。

## 外部依赖

只剩一样是硬的：

| | 用途 | 默认 |
|---|---|---|
| FFmpeg | 成片装配、字幕烧录 | 系统 PATH |

出图出片走进程内的 stable-diffusion.cpp，大模型和配音默认也在进程内
（llama.cpp）。**权重不在包里**——第一次打开界面会让你选，选完它自己下。

大模型想用别的机器上的，把 `[llm].backend` 改成 `remote` 再填地址，
Ollama 和任何兼容 OpenAI 接口的服务都行。配音同理，`[tts].backend = "http"`。

## 配音

默认 `[tts].backend = "local"`：进程内跑，权重是
`[models].tts` 和 `[models].tts_decoder` 两个 GGUF，第一次打开界面时
跟别的模型一起选着下。不用起任何服务。

**出不出得了声，一句话就知道**——不用建项目、不用起服务、不用 ffmpeg：

```bash
changji --say "雨夜的天台上，他没有回头。"
changji --say "试一句" --voice 一段人声.wav      # 参考音色
```

权重没下、或者二进制是不带 llama.cpp 编的，配音会退回估算后端：
只算时长不出声，流水线照样跑得通，成片是静音的。**这一步会在日志里
说一声**，不会假装成功。

要用别的引擎，把后端改成外部 HTTP 服务：

```toml
[tts]
backend = "http"
base_url = "http://某台机器:9880"
```

很多 TTS 项目自带 api 服务，跑在哪台机器上都行。

选型建议见 `docs/`。简单说：要商用无争议就选 Apache 2.0 的
CosyVoice 3 或 Qwen3-TTS，不要用 IndexTTS-2 和 Fish Speech，
前者的商用授权有争议，后者的权重是非商用许可。

**"成功了但没出声"是这一环最阴的故障。** 不少 TTS 实现在内部捕获异常
之后会输出一秒的空音频然后正常返回，状态是成功。场记按时长的绝对下限和
相对下限两条一起拦这种产出，宁可误杀一句"嗯。"，也不让一整集静音文件
被当成配音成功。

## 已知问题

**Docker Desktop 启动后引擎起不来，报 `initializing Inference manager`
或 `initializing Secrets Engine`。**

表现容易误判成「没启动」，其实是启动了但 dockerd 崩了：
`dockerDesktopLinuxEngine` 管道在，但 `/info` 返回 500 空正文；
WSL 里有 `containerd-shim` 却没有 dockerd，`/var/run/docker.sock` 不存在。
真正的报错在 `%LOCALAPPDATA%\Docker\log\host\com.docker.backend.exe.log` 里：

```
backend crashed: starting services: initializing Inference manager:
listening on unix://…\Docker\run\dockerInference: remove …: 系统无法访问此文件
```

原因是残留的 AF_UNIX socket 文件（属性是 `Archive, ReparsePoint`，长度 0）
删不掉——Remove-Item、.NET File.Delete、fsutil 一律返回「系统无法访问此文件」。
Docker 启动时要先 remove 再 listen，remove 失败就崩；崩溃又留下新的 socket，
成了闭环。

处理办法：文件删不掉但父目录能改名。**关键是两个目录必须一起清，然后只启动
一次**——一次只清一个的话，清掉 run 会崩在 secrets，清掉 secrets 又崩回 run，
因为每次失败的启动都会重新造出前一个。

```powershell
Get-Process 'Docker Desktop','com.docker.backend','com.docker.build','docker-sandbox','docker-ai' `
  -ErrorAction SilentlyContinue | Stop-Process -Force
Stop-Service com.docker.service -Force
wsl --shutdown
$stamp = Get-Date -Format 'HHmmss'
foreach ($d in @("$env:LOCALAPPDATA\Docker\run", "$env:LOCALAPPDATA\docker-secrets-engine")) {
    if (Test-Path $d) { Rename-Item $d ((Split-Path $d -Leaf) + ".bak-$stamp") }
    New-Item -ItemType Directory -Path $d -Force | Out-Null
}
Start-Service com.docker.service
Start-Process "$env:ProgramFiles\Docker\Docker\Docker Desktop.exe"
```

注意：设置里的 `EnableDockerAI` 关掉也挡不住——4.72.0 里 Inference manager
照样启动。所以这个坑会复发，复发时照上面处理。

**在 Windows 服务器上通过 SSH 构建镜像会失败。**
报错是 `A specified logon session does not exist`。原因是 Docker Desktop 的
凭据助手要访问 Windows 凭据管理器，而那需要交互式登录会话，SSH 里没有。
改客户端配置没用，凭据是 buildkit 守护进程侧解析的。

两个办法：在远程桌面会话里构建，或者用计划任务把构建投到交互式会话去：

```powershell
schtasks /Create /TN build /TR "powershell -File C:\pathuild.ps1" /SC ONCE /ST 00:00 /RL HIGHEST /IT /F
schtasks /Run /TN build
```

Linux 服务器没有这个问题。

## 许可

Apache-2.0。注意所依赖的各个模型有各自的许可证，部分不可商用，
选型和许可证核查见 `docs/`。
