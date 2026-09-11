/**
 * 接口客户端。
 *
 * 两个前缀：/bff 是 Node 自己的，/api 是转发给 Python 引擎的。
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
    // FastAPI 的报错在 detail 里，Node 这层也统一用 detail
    const detail = data?.detail
    const message =
      typeof detail === 'string'
        ? detail
        : Array.isArray(detail)
          ? detail.map((d) => d.msg || JSON.stringify(d)).join('；')
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
export function mediaUrl(project, rel) {
  if (!project || !rel) return ''
  return '/api/media' + qs({ path: project, rel })
}

export const api = {
  // ---- Node 侧 ----
  nodeHealth: () => get('/bff/health'),
  engineStatus: () => get('/bff/settings/status'),
  settingsOverview: () => get('/bff/settings/overview'),
  nodeConfig: () => get('/bff/settings/config'),
  // 这部剧的画面规格。**一部剧一份**，不是全局设置——一台机器上可以
  // 同时有竖屏短剧和横屏片子。
  projectVideo: (project) => get('/bff/project/video', { path: project }),
  /** 这一轮引擎还没落定的镜头。页面一进来靠它把「排队中」重新点亮。 */
  runPending: () => get('/bff/run/pending'),
  // 大模型跑在哪：内置还是外接。**两条都留着**——本机跑不动大模型的、
  // 想用云上更强模型的、团队共用一台推理机的，都要能切。
  saveLlmBackend: (backend) => post('/bff/settings/llm', { backend }),
  // 首次运行那一页。**四条都在 /bff**：下模型这件事引擎独有，
  // /api 那一套在和 Python 的对拍范围内，加进去就是一处破契约。
  setupState: () => get('/bff/setup/state'),
  startSetupDownload: (payload) => post('/bff/setup/download', payload),
  setupProgress: () => get('/bff/setup/progress'),
  cancelSetupDownload: () => post('/bff/setup/cancel', {}),
  saveProjectVideo: (payload) => post('/bff/project/video', payload),
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
  savePremise: (payload) => post('/api/project/premise', payload),

  // ---- 引擎：剧本 ----
  writeScript: (payload) => post('/api/script/write', payload),
  suggestPremises: (payload) => post('/api/script/premise', payload),
  writeTrailer: (payload) => post('/api/script/trailer', payload),
  seriesStatus: () => get('/api/script/series'),
  writeSeries: (payload) => post('/api/script/series', payload),
  stopSeries: () => post('/api/script/series/stop', {}),
  getScript: (path, episodeId) =>
    get('/api/script', { path, episode_id: episodeId }),
  saveScript: (payload) => post('/api/script', payload),

  // ---- 引擎：故事 ----
  //
  // 写和采用是两个接口：/outline 只回草稿不落库，人点了采用才走 /adopt。
  // 源头没人审过就往下跑，后面几十分钟的渲染全是白跑。
  getStory: (path) => get('/api/story', { path }),
  saveStory: (payload) => post('/api/story', payload),
  writeOutline: (payload) => post('/api/story/outline', payload),
  adoptStory: (payload) => post('/api/story/adopt', payload),
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
  reorderShots: (payload) => post('/api/shots/reorder', payload),
  newEpisode: (payload) => post('/api/episode', payload),
  episodeAction: (payload) => post('/api/episode/action', payload),

  // ---- 引擎：制作 ----
  runPreview: (params) => get('/api/run/preview', params),
  runStatus: () => get('/api/run'),
  run: (payload) => post('/api/run', payload),
  stopRun: () => post('/api/stop', {}),
  outputs: (path) => get('/api/outputs', { path }),
  hardware: () => get('/api/hardware'),
  // 此刻的负载，一次性的。顶栏那三个小表走 WebSocket（订 "system"），
  // 这个留给排查用。
  system: () => get('/api/system'),
  doctor: () => get('/api/doctor'),
  connections: () => get('/api/connections'),
  saveConnections: (payload) => post('/api/connections', payload),
  engineSettings: () => get('/api/settings'),
  saveEngineSettings: (payload) => post('/api/settings', payload),
}
