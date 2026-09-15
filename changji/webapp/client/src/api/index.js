/**
 * 接口客户端。
 *
 * 两个前缀，**今天是同一个进程在答**：`/api` 是引擎自己的接口，`/bff` 是
 * 给界面拼好的那几条（一次问完一屏要的东西）。`/bff` 这个名字是历史：
 * webapp 原来跑在 Node 那层后面，那几条是那层的；2026-09-12 把前端嵌进
 * 二进制之后由 C++ 自己答（清单在 cpp/src/http/bff_routes.hpp，
 * test_webapp.cpp 拿打包进来的前端代码里出现的 /bff/ 路径查那份清单）。
 *
 * 前端不关心谁在后面，只关心报错时能拿到一句人能看懂的话。
 */

export class ApiError extends Error {
  constructor(message, status, payload) {
    super(message)
    this.status = status
    this.payload = payload
  }
}

async function request(url, { method = 'GET', body, signal, raw } = {}) {
  let res
  try {
    res = await fetch(url, {
      method,
      headers: body instanceof FormData || body === undefined
        ? undefined
        : { 'content-type': 'application/json' },
      body:
        body === undefined
          ? undefined
          : body instanceof FormData
            ? body
            : JSON.stringify(body),
      signal,
    })
  } catch (err) {
    if (err.name === 'AbortError') throw err
    throw new ApiError('网络请求发不出去，检查一下服务是不是还开着', 0)
  }

  if (raw) return res

  const text = await res.text()
  let data = null
  if (text) {
    try {
      data = JSON.parse(text)
    } catch {
      data = { detail: text.slice(0, 300) }
    }
  }
  if (!res.ok) {
    // 报错一律在 detail 里。**这是照着 Python 那版的形状定的**，引擎
    // 现在也照发（见下面 fieldError 那段说的 pydantic 形状），对拍语料
    // 把它钉住了。
    const detail = data?.detail
    const message =
      typeof detail === 'string'
        ? detail
        : Array.isArray(detail)
          ? detail.map(fieldError).join('；')
          : `请求失败（${res.status}）`
    throw new ApiError(message, res.status, data)
  }
  return data ?? {}
}

function qs(params) {
  const usable = Object.entries(params || {}).filter(
    ([, v]) => v !== undefined && v !== null && v !== '',
  )
  if (!usable.length) return ''
  return '?' + new URLSearchParams(usable).toString()
}

const get = (url, params, opts) => request(url + qs(params), opts)
const post = (url, body, opts) => request(url, { ...opts, method: 'POST', body })
const del = (url, opts) => request(url, { ...opts, method: 'DELETE' })

/** 媒体文件的地址。给 img / video / audio 的 src 用。 */
/**
 * 422 里的一条，翻成人能看懂的一句。
 *
 * **字段名在 `loc` 里，不在 `msg` 里。** 引擎照 pydantic 的形状发
 * （`{type, loc:["body", 字段], msg, input}`，见 readonly.hpp 的
 * `unprocessable_top`），而 msg 是那套固定的英文短语——「Extra inputs are
 * not permitted」「Input should be a valid number」。只取 msg 的话，用户
 * 看到的是**一句没有主语的英文**：多传了哪个字段、哪个数不合法，一个字
 * 都没有。而这一族错误的全部信息量就在字段名上。
 *
 * `loc` 的第一段固定是 "body"（pydantic 用来分 body / query / path），
 * 对用户没意义，去掉；剩下的用点连起来，正好是引擎那边写的
 * 「patch.preset」这种。
 */
function fieldError(d) {
  if (!d || typeof d !== 'object') return String(d)
  const msg = d.msg || JSON.stringify(d)
  const where = Array.isArray(d.loc)
    ? d.loc.filter((x) => x !== 'body' && x !== 'query' && x !== 'path').join('.')
    : ''
  return where ? `${where}：${msg}` : msg
}

export function mediaUrl(project, rel) {
  if (!project || !rel) return ''
  return '/api/media' + qs({ path: project, rel })
}

export const api = {
  // ---- /bff：给界面拼好的那几条 ----
  //
  // 名字是历史（原来在 Node 那层），现在和 /api 一样由引擎答。
  engineStatus: () => get('/bff/settings/status'),
  /**
   * 设置页那一整页。里面嵌着 node / engine / connections / settings /
   * hardware / doctor 六块。
   *
   * **把项目路径带上**，理由和下面 `doctor` 那条一模一样：这份里的体检
   * 有一项是「出片画布」，而画幅是每部剧自己的（项目目录的 changji.toml
   * 里那个 [video]）。不带的话引擎查的是全局默认（竖屏 720p，544×928），
   * 那个数永远不会超上限——项目切到 2K 之后，设置页第一节照样说没问题、
   * 上面那颗牌子照样写「可以开工」，而镜头页开跑前的那次体检
   * （走 /api/doctor，带了 path）会说超了。同一条检查两个答案。
   */
  settingsOverview: (project) =>
    get('/bff/settings/overview', project ? { path: project } : {}),
  // **`/bff/settings/overview` 一条就够。** 它里面已经嵌了 node（就是
  // /bff/settings/config 那份）、engine、connections、settings、hardware
  // 五块，引擎那边是进程内直接取、不发 HTTP（见 server.cpp 那段注释）。
  // 单独再包 `nodeHealth` `nodeConfig` `engineSettings` `hardware` 四个
  // 函数的话，同一份数据有两条取法，而设置页走的是 overview 这条——
  // 另一条只会被将来某个人捡起来用，然后两处显示的东西开始不一样。
  // 四个都没人叫，删掉；接口本身留着（curl 排查、别的客户端）。
  // 这部剧的画面规格。**一部剧一份**，不是全局设置——一台机器上可以
  // 同时有竖屏短剧和横屏片子。
  projectVideo: (project) => get('/bff/project/video', { path: project }),
  /** 这一轮引擎还没落定的镜头。页面一进来靠它把「排队中」重新点亮。 */
  runPending: () => get('/bff/run/pending'),
  // 「大模型跑在哪：内置还是外接」那条（POST /bff/settings/llm）**没有了**。
  // 进程内那条 2026-09-14 删掉之后引擎只收 remote 一个值，这个函数也就一直
  // 没人叫——而它头上那段注释还写着"两条都留着，都要能切"，读代码的人会
  // 去界面上找那个开关。引擎那条路线留着（老机器上 backend = "local" 的配置
  // 靠它改回来，见 server.cpp 那段），界面这边不留一个没人用的入口。

  // 首次运行那一页。**四条都在 /bff**：下模型这件事引擎独有，
  // /api 那一套在和 Python 的对拍范围内，加进去就是一处破契约。
  /**
   * 首次运行那一页、以及项目页那个模型窗口读的都是它。
   *
   * **带上项目**：挑了哪一档记在项目的 changji.toml 里（`[models.pick]`），
   * 不带的话这条只看全局——人在项目页挑完，再打开那个窗口看到的还是全局
   * 那一档，看着像没保存上。没有项目时不带，行为和以前一样。
   */
  setupState: (project) => get('/bff/setup/state', project ? { path: project } : {}),
  startSetupDownload: (payload) => post('/bff/setup/download', payload),
  setupProgress: () => get('/bff/setup/progress'),
  cancelSetupDownload: () => post('/bff/setup/cancel', {}),
  saveProjectVideo: (payload) => post('/bff/project/video', payload),
  /** 成片工序：后期链（[look]）和声音几层（[sound]）。剧的属性。 */
  projectFinish: (project) => get('/bff/project/finish', { path: project }),
  saveProjectFinish: (payload) => post('/bff/project/finish', payload),
  saveNodeConfig: (patch) => post('/bff/settings/config', patch),
  flow: (project, episodeId) =>
    get('/bff/flow', { path: project, episode_id: episodeId }),
  platforms: () => get('/bff/publish/platforms'),
  publishTargets: () => get('/bff/publish/targets'),
  savePublishTarget: (target) => post('/bff/publish/targets', target),
  deletePublishTarget: (id) => del('/bff/publish/targets/' + encodeURIComponent(id)),
  publishRecords: (project) => get('/bff/publish/records', { project }),
  deliver: (payload) => post('/bff/publish/deliver', payload),
  deliverBatch: (payload) => post('/bff/publish/batch', payload),

  // ---- 引擎：项目 ----
  projects: () => get('/api/projects'),
  project: (path) => get('/api/project', { path }),
  newProject: (payload) => post('/api/new', payload),
  deleteProject: (payload) => post('/api/project/delete', payload),
  /** 改剧名。只动 project.json 的 title，目录不搬——目录名是项目的身份。 */
  renameProject: (payload) => post('/api/project/rename', payload),
  // 「只改梗概」那条（POST /api/project/premise）**这儿不留绑定**。
  //
  // 它只写 project.json 的 premise，而梗概在盘上有**两份**：story.json 里
  // 那份是故事页编辑的，project.json 里那份是老流程写剧本的提示词读的。
  // `/api/story` 是唯一会把两份一起对上的入口（见 post_story：「两边各存
  // 一份的话，在故事页改完梗概、去写剧本用的还是旧的那句」），故事页走的
  // 就是它。留一个"看起来更专一"的绑定在这儿，下一个人会顺手挑它，然后
  // 得到两份对不上的梗概——不报错，只是写剧本用的是旧那句。
  //
  // 引擎那条路线留着：它在和 Python 的对拍范围内，删了就是破契约。

  // ---- 引擎：剧本 ----
  writeScript: (payload) => post('/api/script/write', payload),
  suggestPremises: (payload) => post('/api/script/premise', payload),
  writeTrailer: (payload) => post('/api/script/trailer', payload),
  seriesStatus: () => get('/api/script/series'),
  /**
   * 一次把所有集的剧本写出来。
   *
   * ⚠️ **界面上今天没有入口——这是全仓库两个"包了没人叫"之一**（另一个是
   * 下面的 `dedupeAssets`；90 个包装里就这两个）。引擎那一头是齐的
   * （`POST /api/script/series`，状态和停都在下面两条上，而那两条**有人
   * 叫**：故事页的「展开」走的是 `/api/story/chapters`，和这条共用「写」
   * 那个作业槽，所以停和轮询顺带就通了）。
   *
   * 也就是说今天的状态是：**能停、能看进度，就是起不来。** 留着不删——
   * 删了将来要接回去得连引擎那侧一起重新对一遍；而 index.js 上面那次删
   * 四个包装（`nodeHealth` 那几个）是另一回事，那四个和 overview 重复，
   * 这个不重复，它是这个功能唯一的客户端路径。
   */
  writeSeries: (payload) => post('/api/script/series', payload),
  stopSeries: () => post('/api/script/series/stop', {}),
  getScript: (path, episodeId) =>
    get('/api/script', { path, episode_id: episodeId }),
  // 这一集的原料：分集表压着的场、原文切片、钩子、四段按秒的排法、对白预算。
  // 和 AI 改编时拿到的是同一份，只是给人看。
  getScriptContext: (path, episodeId) =>
    get('/api/script/context', { path, episode_id: episodeId }),
  saveScript: (payload) => post('/api/script', payload),

  // ---- 引擎：故事 ----
  //
  // 写和采用是两个接口：/outline 只回草稿不落库，人点了采用才走 /adopt。
  // 源头没人审过就往下跑，后面几十分钟的渲染全是白跑。
  getStory: (path) => get('/api/story', { path }),
  saveStory: (payload) => post('/api/story', payload),
  writeOutline: (payload) => post('/api/story/outline', payload),
  adoptStory: (payload) => post('/api/story/adopt', payload),
  // 丢掉还没采用的那份大纲。**草稿是落库的**，所以「丢弃」不能只清
  // 浏览器里那个 ref——不清服务端那份的话刷新一下它又回来了。
  dropStoryDraft: (payload) => post('/api/story/draft/drop', payload),
  planEpisodes: (payload) => post('/api/story/plan', payload),
  // 分集表是计划，这一步才把它变成流水线真正在跑的剧集。
  // 已有的同号剧集只补元数据，写好的剧本和出过的片一个字不动。
  makeEpisodes: (payload) => post('/api/story/episodes', payload),
  // 粘进来的文本切成章节。不碰大模型，切章节是机械活。
  importStory: (payload) => post('/api/story/import', payload),
  // 读一遍已经存下的正文，把人物关系地点和真钩子提出来。正文不动。
  analyzeStory: (payload) => post('/api/story/analyze', payload),
  // 展开一章的正文。**这个是直接落库的**，不回草稿——它只往空字段里填
  // 东西，而十六章走草稿-采用就是三十二次点击。
  writeChapter: (payload) => post('/api/story/chapter', payload),
  /** 删一章。分集表跟着改，回包里说改了几条。 */
  deleteChapter: (payload) => post('/api/story/chapter/delete', payload),
  // 一口气展开所有还没正文的章。走长跑作业，进度在 seriesStatus 里，
  // 和「写整季」共用同一个任务槽。
  writeChapters: (payload) => post('/api/story/chapters', payload),
  // 选中一段让 AI 改。**只回草稿**——改稿落错了盖掉的是作者自己写的字。
  reviseStory: (payload) => post('/api/story/revise', payload),
  // 把改好的那一段写回去。不碰大模型，纯字符串替换 + 重算切点和分集。
  applyRevision: (payload) => post('/api/story/revise/apply', payload),
  // 老项目：从已有剧集反推一份故事骨架。不碰大模型，也不重新分集。
  storyFromEpisodes: (payload) => post('/api/story/from_episodes', payload),
  // 念一段字出来。**一次最多两百字**（模型一次合成的上限约 41 秒），
  // 超了回 truncated:true，界面照实说。
  say: (payload) => post('/api/tts/say', payload),

  // ---- 引擎：角色 / 场景 / 风格 ----
  assets: (path) => get('/api/assets', { path }),
  makeBible: (payload) => post('/api/bible', payload),
  saveCharacter: (payload) => post('/api/character', payload),
  clearReference: (payload) => post('/api/character/reference/clear', payload),
  // 参考音色：一段人声片段，进程内配音照着它的音色念。**没有服务端的
  // 音色清单**，所以"选音色"这件事就是"给一段参考音频"，见 /api/voices。
  uploadCharacterVoice: (form) =>
    request('/api/character/voice', { method: 'POST', body: form }),
  clearCharacterVoice: (payload) => post('/api/character/voice/clear', payload),
  // 「制作音色」：不给参考音频时种子决定说话人，摇一个试听，满意了存下来。
  // **存下来之后它就是一段普通的参考音频**，从此被克隆锁死，不会再变。
  voicePresets: () => get('/api/voice/presets'),
  voiceTake: (payload) => post('/api/voice/take', payload),
  voiceSave: (payload) => post('/api/voice/save', payload),
  uploadReference: (form) =>
    request('/api/character/reference', { method: 'POST', body: form }),
  // 照着设定里那段外观描述现画一张。**一次一张，同步，几十秒**——头一张
  // 还要先把出图模型读进显存，那段时间一点动静都没有。调用处必须自己
  // 摆一个"正在画"的状态，否则用户会连点。
  generateReference: (payload) =>
    post('/api/character/reference/generate', payload),
  saveLocation: (payload) => post('/api/location', payload),
  uploadLocationReference: (form) =>
    request('/api/location/reference', { method: 'POST', body: form }),
  clearLocationReference: (payload) => post('/api/location/reference/clear', payload),
  generateLocationReference: (payload) =>
    post('/api/location/reference/generate', payload),
  saveStyle: (payload) => post('/api/style', payload),
  voices: (path) => get('/api/voices', { path }),
  llmModels: () => get('/api/llm/models'),
  llmProviders: () => get('/api/llm/providers'),

  // ---- 引擎：分镜 ----
  plan: (payload) => post('/api/plan', payload),
  planAll: (payload) => post('/api/plan/all', payload),
  shots: (path, episodeId) => get('/api/shots', { path, episode_id: episodeId }),
  saveShot: (payload) => post('/api/shot', payload),
  batchShots: (payload) => post('/api/shots/batch', payload),
  linkLocations: (payload) => post('/api/shots/link_locations', payload),
  /**
   * 把同名的场景/角色收成一条。不叫模型，秒回。
   *
   * ⚠️ **界面上今天没有入口**，同 `writeSeries`（全仓 90 个包装里只有这
   * 两个没人叫）。设定页那两格能看出重复——`character.hpp` 里那段就记着
   * 实测撞到的例子：一部剧里三个 id 指着同一个后台
   * （`loc_..._auditorium_backstage` / `..._old_stage_backstage` /
   * `loc_old_stage_backstage_daytime`）——但收不了。
   */
  dedupeAssets: (payload) => post('/api/assets/dedupe', payload),
  reorderShots: (payload) => post('/api/shots/reorder', payload),
  newEpisode: (payload) => post('/api/episode', payload),
  episodeAction: (payload) => post('/api/episode/action', payload),

  // ---- 引擎：制作 ----
  runPreview: (params) => get('/api/run/preview', params),
  runStatus: () => get('/api/run'),
  run: (payload) => post('/api/run', payload),
  stopRun: () => post('/api/stop', {}),

  /**
   * 把某一件正在后台跑的活停掉（写大纲、写正文、写剧本、拆分镜这一族）。
   *
   * **按 stream 停，不按种类停。** stopRun / stopSeries 停的是"出片"和
   * "写整季"那两个长跑任务，一种只有一个槽；这一族是按请求起的，同时可以
   * 有好几件，只能按它自己那条 stream 认。
   *
   * 找不到不是错（按下去那一刻可能刚好干完），回的是 {stopped: false}。
   */
  cancelJob: (stream) => post('/api/job/cancel', { stream }),

  /**
   * 开一个信箱：这条 stream 上的消息除了走 WebSocket，再往服务端存一份。
   *
   * **给连不上 WebSocket 的场合用的**（代理掐了 Upgrade、页面刚打开）。
   * 开完照样带 `async` 发请求，然后拿 jobEvents 一条条取回来——进度、
   * 思考、顶栏那个「停下」就都还在。见 composables/useAsyncJob。
   *
   * **要在发那个请求之前开。** 反过来的话开之前那几条没地方存，表现为
   * "前面一截思考不见了"，而第一段思考往往就在那几百毫秒里。
   */
  watchJob: (stream) => post('/api/job/watch', { stream }),

  /**
   * 取走这条 stream 上 `since` 之后的消息。
   *
   * 回 `{events, next, done, dropped, exists}`。`exists` 为假是"信箱没了"
   * ——没开过，或者太久没来取被扫掉了；按"连接断了"处理，别傻等。
   */
  jobEvents: (stream, since) => get('/api/job/events', { stream, since }),
  outputs: (path) => get('/api/outputs', { path }),
  // 那张「机器 × 能力」的表。**答得慢是正常的**：引擎要挨个问
  // 别的机器的 /status（每台最多 3 秒），结果缓存五秒。
  nodes: () => get('/api/nodes'),
  // 关掉／打开某台的某个能力。**正在跑的时候会被拒（409）**：
  // 半集换机器会让前后画风对不上。
  setNodeOff: (url, cap, off) => post('/api/nodes/off', { url, cap, off }),
  // 任意一台机器的模型：本机走本地那份，别的机器由引擎转发过去。
  // **浏览器连不上那几台**（地址可能只有引擎这边通，口令也不该发到前端），
  // 所以这几条都带一个 url 参数走引擎。
  //
  // （合并时没把分支上那条 `hardware: () => get('/api/hardware')` 带进来：
  //   这一版的硬件信息由 `/bff/settings/overview` 一次带回来，单独那条
  //   全仓一个调用点都没有。NodeMatrix 用的是上面 nodeSetup 那几条。）
  nodeSetup: (url) => get('/api/nodes/setup', { url }),
  nodeSetupDownload: (url, selections) =>
    post('/api/nodes/setup/download', { url, selections }),
  nodeSetupProgress: (url) => get('/api/nodes/setup/progress', { url }),
  nodeSetupCancel: (url) => post('/api/nodes/setup/cancel', { url }),
  // 此刻的负载，一次性的。顶栏那三个小表走 WebSocket（订 "system"），
  // 这个留给排查用。
  system: () => get('/api/system'),
  /**
   * 体检。**把项目路径带上。**
   *
   * 其中「出片画布」那一项查的是 `[video].quality` 算出来的宽高，而画幅是
   * **每部剧自己的**。不带 path 的话引擎查的是全局默认（竖屏 720p，
   * 544×928），那个数永远不会超上限——项目切到 2K 之后体检照样说没问题，
   * 这条检查等于没有。引擎那头 2026-09-14 就收这个参数了（见
   * `/api/doctor` 路由上那段注释：「实测撞到过」），界面一直没给。
   */
  doctor: (project) => get('/api/doctor', { path: project }),
  /**
   * 列一个目录下面的子目录。**只回目录**——这条唯一的用处是挑一个放模型
   * 的文件夹，把几百个权重文件一起列出来只会把目录淹掉。
   *
   * 走的是**引擎那台**的文件系统。引擎跑在别的机器上时，浏览器里看到的
   * 自然也是那台的盘——这正是要的：模型下到哪儿是那台说了算。
   */
  listDirs: (path) => get('/api/fs/dirs', { path }),
  connections: () => get('/api/connections'),
  saveConnections: (payload) => post('/api/connections', payload),
  saveEngineSettings: (payload) => post('/api/settings', payload),
}
