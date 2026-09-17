# 改这个仓库之前先看这份

场记 changji：把一堆生成模型串成短剧生产线的编排层，**一个二进制**
（界面 + 接口 + 编排 + 出图 + 出片 + 配音 + 大模型全在进程内）。
这份文件是**索引 + 容易踩错的地方**，长篇说明在 [docs/](docs/)，
产品介绍在 [README.md](README.md)。

## 代码地图

```
cpp/src/
  main.cpp            入口、命令行（--doctor / --say / --port / --worker）
  http/               路由和接口。server.cpp 是总路由表；
                      readonly/projects/episodes/planning/scripting/batch/run
                      /config_api 各管一摊；run_deps.cpp 组装后端（池、farm）
  pipeline/           一集怎么跑：episode.cpp（阶段编排）、jobs.hpp（作业表、
                      进度、取消）、shot_flow.hpp（首帧和出片的排位）
  stages/             每一步的纯逻辑：story_outline / story_analyze /
                      chapter_write / script_story / storyboard / frames /
                      render / audio / bible / assemble
  infer/              派活：worker_pool（池）、worker_farm（本机多卡拉子进程）、
                      worker_server（工作进程那套接口）、node_registry（机器表）
  llm/                大模型客户端。**schema 以文字贴在提示词后面**，见下
  media/              时间轴、切集、ffmpeg 参数
  models/             Shot / Project / AssetLibrary / Story 这些数据结构
  gates/              质量闸门（硬切、时长、响度、参考图）
  config/             配置读写。机器的设置和剧的设置分两处
cpp/prompts.toml      所有提示词。构建时由 tools/gen_prompts.py 生成成头文件
cpp/tests/unit/       doctest。**基线是 4 个红**，见下
webapp/client/        Vue 3。打包后嵌进二进制（tools/gen_webapp.py）
```

## 常用命令

```bash
# 本机（仓库在 changji/，构建目录在它外面一层）
cmake --build ../build -j8                      # 引擎
cmake --build ../build_tests -j8                # 单元测试
../build_tests/changji_tests                    # 跑全部；-tc="用例名" 跑一条
(cd webapp/client && npm run build) && python3 cpp/tools/gen_webapp.py   # 改了前端要这两步
pkill -f 'build/changji [-]-port'
(cd .. && nohup ./build/changji --port 8080 > /tmp/changji8080.log 2>&1 &)
```

- **macOS 没有 `setsid`**，本机起服务用 `(nohup … &)`。
- 改了 `webapp/` 一定要**重新打包再 gen_webapp 再编引擎**，否则跑的是旧界面。
  `npm run build` 会先跑 eslint，**有 error 时 vite 根本不构建**——只看
  "embedded" 没看 lint 的话会把旧包嵌进去。

## 测试基线

全量 **1148 条左右，其中 4 条红**，都是和 Python 老实现逐字节对拍的语料：

```
两个出片接口和 Python 逐条对拍
提示词和 Python 逐字节一致
提示词组装和 Python 逐字节一致
解析出的镜头和 Python 一致
```

提示词这一年改了很多，这几份语料早就跟不上了。**它们要不要退役是待定的
产品决定**（用户还没拍板），在那之前：

- 别去"修"它们，也别把它们当回归信号；
- **但也别再往它们钉着的东西上改**——`test_bible.cpp` 的
  「schema 结构和 Python 一致」现在是**绿的**，它把 bible 的 schema 也钉住了。

## 最容易踩错的地方

### 一、没有语法层

2026-09-14 起**结构约束整个交给提示词**：本地 GBNF 随本地后端删了，
`response_format` 也不发（各家支持得七零八落）。`llm::schema_as_prompt`
把 schema **当文字贴在提示词后面**。所以：

- `enum` / `required` / `minItems` / `maxItems` 对远端模型**只是建议**，
  `schema_validate.hpp` 里写着「这些模型根本不按 GBNF 生成」；
- 说"这条规则 schema 管着了"时，意思只是"同样的话在 schema 那半截里也有"，
  **不是"它没得选"**；
- schema 是提示词里最大的一块（分镜那份贴过去 5600+ 字符，规则表才 859），
  改它就是改约束本身。

### 二、砍提示词的判据（三条，缺一条就会砍错）

1. **字段描述只管得住那一栏。** 要管所有栏的规则（「哪一栏都不要写长相」）
   留在规则表里——实测长相会写进 `summary`。
2. **搬进 description 之前先看那份 schema 有没有对拍语料钉着**（bible 的
   schema 就钉着，动它当场红）。
3. **只有这一次调用自己收得到的东西才算"已经管着了"**。拿另一个阶段的
   schema 当理由是错的——那是另一次调用，根本收不到。

### 三、量比例再动手

说"太臃肿"时先量组成。2026-09-17 的实证：砍规则表一整天省 1294 字，而
schema 三笔（缩进 2→1、摘掉没人引用的 `$defs`、把重复的 beat 定义提成
`$defs` 各处 `$ref`）省了四千多，**一个字的约束都没丢**。
**臃肿长在没人看的地方**（`dump()` 出来的 schema、成片目录里的文件）。

查质量问题同理：从**产出物往回查**（`output/`、`subtitles/` 里的文件名、
时长、数量）比从代码往前读快得多。

### 四、引用规矩引原话，别引条号

提示词那几张表一砍就重编号，注释里写「第 14 条」当场指歪。原话能 grep，
条号不能。（`storyboard.cpp` 上就写着这条，2026-09-17 又犯了一次。）

### 五、并行那一段一个字节都不往 Shot 里写

`frames` / `render` / `audio` 三层都是"各路跑副本 → 收完在调用线程上按原
顺序写回"。写回的凭据是 **`Done::ran`（正面判据）**，不是"没被标成跳过"
——2026-09-17 就是靠后者丢了一整集：某条路径提前 return，一格都没标跳过，
收尾时把 17 镜全写成了默认构造的空壳（`shot_id` 变空串）。

### 六、多机互联

**一台机器 = 一个 `changji` 进程 = 机器表里一条 `[[peer.nodes]]`**，指主
服务端口。那个进程自己按显卡数拉起 `--worker --gpu N` 子进程（`worker_farm`），
外面只连主端口。别手工起 worker、别一台机器连两次。

`/status` 里报 `slots`（= 活着的子进程数），派活那头按它开槽
（`infer::remote_slots_for`）——不报的话一台双卡机只会被当成一个槽。

## 远端那台（有显卡的）

```
root@43.110.148.239   两张 NVIDIA L20（46 GB）· 32 核
仓库 /root/changji-src/changji/      构建 /root/changji-src/build
```

```bash
rsync -rlz cpp/src/  root@…:/root/changji-src/changji/cpp/src/     # 不要用 -a
ssh root@… 'export PATH=/usr/local/cuda/bin:$PATH; cd /root/changji-src && cmake --build build -j32'
ssh root@… "strings build/changji | grep -c cublas"                # 必须 > 0
ssh root@… "pkill -9 -f 'changji [-]-host'"                        # 和启动分两次 ssh
ssh root@… "cd /root/changji-src && (setsid nohup ./build/changji --host 0.0.0.0 --port 8080 > /root/changji.log 2>&1 &)"
```

- **`pkill` 和启动必须分两条 ssh 命令**，写在一起会把自己那个 shell 也匹配掉（退出码 255）。
- GPU 构建要 `CHANGJI_SD_CUDA=ON` / `CHANGJI_CUDA_ARCH=89`。
- 远端 `/status` 有口令门，直接 curl 是 401；看本机 `/api/nodes` 里那一行更省事。

## 工作习惯

- **验证顺序：页面 → 本机后台 → 远程。** 只看接口会漏掉崩溃和 GPU 空转；
  编译重启完把 http://127.0.0.1:8080 打开，导到这次改动看得见的那一页。
- **能自动解决的就别报错。**
- **只 commit，不 push。** push 只在当次明确要求时做。
