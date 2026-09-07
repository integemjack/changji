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
  saveNodeConfig: (patch) => post('/bff/settings/config', patch),
  flow: (project, episodeId) =>
    get('/bff/flow', { path: project, episode_id: episodeId }),
  platforms: () => get('/bff/publish/platforms'),
  publishTargets: () => get('/bff/publish/targets'),
  savePublishTarget: (target) => post('/bff/publish/targets', target),
  deletePublishTarget: (id) => del('/bff/publish/targets/' + encodeURIComponent(id)),
  publishRecords: (project) => get('/bff/publish/records', { project }),
  deliver: (payload) => post('/bff/publish/deliver', payload),

  // ---- 引擎：项目 ----
  projects: () => get('/api/projects'),
  project: (path) => get('/api/project', { path }),
  newProject: (payload) => post('/api/new', payload),
  deleteProject: (payload) => post('/api/project/delete', payload),

  // ---- 引擎：剧本 ----
  writeScript: (payload) => post('/api/script/write', payload),
  seriesStatus: () => get('/api/script/series'),
  writeSeries: (payload) => post('/api/script/series', payload),
  stopSeries: () => post('/api/script/series/stop', {}),
  getScript: (path, episodeId) =>
    get('/api/script', { path, episode_id: episodeId }),
  saveScript: (payload) => post('/api/script', payload),

  // ---- 引擎：角色 / 场景 / 风格 ----
  assets: (path) => get('/api/assets', { path }),
  makeBible: (payload) => post('/api/bible', payload),
  saveCharacter: (payload) => post('/api/character', payload),
  clearReference: (payload) => post('/api/character/reference/clear', payload),
  uploadReference: (form) =>
    request('/api/character/reference', { method: 'POST', body: form }),
  saveLocation: (payload) => post('/api/location', payload),
  saveStyle: (payload) => post('/api/style', payload),
  voices: (path) => get('/api/voices', { path }),

  // ---- 引擎：分镜 ----
  plan: (payload) => post('/api/plan', payload),
  planAll: (payload) => post('/api/plan/all', payload),
  shots: (path, episodeId) => get('/api/shots', { path, episode_id: episodeId }),
  saveShot: (payload) => post('/api/shot', payload),
  batchShots: (payload) => post('/api/shots/batch', payload),
  newEpisode: (payload) => post('/api/episode', payload),
  episodeAction: (payload) => post('/api/episode/action', payload),

  // ---- 引擎：制作 ----
  runPreview: (params) => get('/api/run/preview', params),
  runStatus: () => get('/api/run'),
  run: (payload) => post('/api/run', payload),
  stopRun: () => post('/api/stop', {}),
  outputs: (path) => get('/api/outputs', { path }),
  hardware: () => get('/api/hardware'),
  doctor: () => get('/api/doctor'),
  connections: () => get('/api/connections'),
  saveConnections: (payload) => post('/api/connections', payload),
  engineSettings: () => get('/api/settings'),
  saveEngineSettings: (payload) => post('/api/settings', payload),
}
