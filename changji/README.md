# 场记 changji

AI 短剧生产流水线。从剧本到成片的本地编排引擎，支持真人与动漫双风格。

名字取自影视工作里的「场记」，这个岗位负责的正是这套软件要做的事：
维护分镜表、盯住跨镜头的连续性、记录每一条的状态。

## 这是什么

它不是生成模型，是把一堆生成模型串成生产线的编排层。
真正的推理交给 ComfyUI，大模型推理交给 Ollama 或任何兼容 OpenAI 接口的服务。
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
- ComfyUI 是配置里的一个地址，可以指向局域网任意一台有显卡的机器
- 画质档位由运行时探测到的显存推导，不写死
- 配置优先级：环境变量 > 项目配置 > 用户全局配置 > 默认值

## 安装

需要 Python 3.11 或更高版本。

```bash
pip install -e .
```

首次运行前生成配置模板：

```bash
changji init
```

## 依赖的外部服务

| 服务 | 用途 | 默认地址 |
|---|---|---|
| ComfyUI | 出图、图生视频、口型 | http://127.0.0.1:8188 |
| Ollama 或兼容服务 | 剧本、分镜 | http://127.0.0.1:11434/v1 |
| FFmpeg | 成片装配 | 系统 PATH |

三者都可以在别的机器上，改配置即可。

## 接入真实配音

默认的配音后端只算时长不出声音，用途是让流水线在没装 TTS 的机器上也能
跑通。要出真实语音，把 TTS 跑在 ComfyUI 那台机器上。

先看服务端有哪些可用节点：

```bash
changji nodes tts
```

ComfyUI 自带的 TTS 节点全是云 API，要联网和密钥。本地方案需要装节点包，
社区生态已收敛到 TTS-Audio-Suite，它覆盖十几个引擎：

```bash
cd ComfyUI/custom_nodes
git clone https://github.com/diodiogod/TTS-Audio-Suite.git
cd TTS-Audio-Suite && python install.py
```

装完重启 ComfyUI，再跑一次 `changji nodes tts` 确认节点已加载。

然后在 ComfyUI 界面里搭一个配音工作流，导出保存到项目的
`workflows/tts.json`。场记会自动识别并启用它，不需要改配置。

工作流里的文本节点参数名可以是 text、prompt、input_text、tts_text 或
content 中的任意一个，音色可以是 voice、voice_id、speaker 或
reference_audio，情绪可以是 emotion、style 或 instruct。场记按键名匹配，
所以换引擎通常不用改代码。

选型建议见 `docs/`。简单说：要商用无争议就选 Apache 2.0 的
Qwen3-TTS 或 CosyVoice 3，不要用 IndexTTS-2 和 Fish Speech，
前者的商用授权有争议，后者的权重是非商用许可。

### 配音引擎的兼容性

默认用 CosyVoice 3，Apache 2.0 可商用，已实测跑通。

不要换成 Qwen3-TTS：它在 transformers 5.x 下加载失败，报
`Failed to load Qwen3-TTS model: 'default'`。模型 config 声明的是
4.57.3，节点代码没跟上 5.x 的接口变化，而 ComfyUI 本身需要 5.x，
不能靠降级解决。

也不要换成 IndexTTS 或 Fish Speech，前者商用授权有争议，
后者权重是非商用许可。

这类失败有个共同表现：节点内部捕获异常后输出一个一秒的空音频并正常
返回，ComfyUI 报的任务状态是 success。场记会检测并拦下这种空音频，
但排查时要直接看 ComfyUI 的日志才能知道真正原因。

同一个节点包里还有 CosyVoice、IndexTTS、F5TTS 等引擎，
换 `workflows/tts.json` 里的引擎节点即可，不用改代码。

## 已知问题

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
