/**
 * ComfyUI 客户端。
 *
 * 用 WebSocket 监听进度而不是轮询历史接口，因为成片档一个镜头要跑好几分钟，
 * 轮询既慢又容易漏掉中间状态。
 *
 * 失败分两类，处理方式完全不同：
 * 提交时的校验失败（模型文件缺失、参数越界）是确定性的，重试没有意义，直接抛。
 * 执行中的失败（显存不足、节点崩溃）可能是偶发的，值得重试。
 */

import fs from 'node:fs'
import path from 'node:path'
import { Readable } from 'node:stream'
import { pipeline } from 'node:stream/promises'
import { randomUUID } from 'node:crypto'

import { comfyWsUrl } from '../config.js'

/**
 * WebSocket 静默多久就去查一次历史兜底。
 * 查得太勤给服务端添负担，太懒则任务早跑完了界面还在等。
 */
const RECHECK_EVERY_MS = 5000

export class ComfyError extends Error {}

/** 连不上。地址错了、服务没起、或者网络不通。 */
export class ComfyUnavailable extends ComfyError {}

/**
 * 提交时被服务端拒绝。重试没有意义。
 *
 * 最常见的原因是工作流引用的模型文件在服务端不存在。
 */
export class PromptValidationError extends ComfyError {
  constructor(message, nodeErrors = null) {
    super(message)
    this.nodeErrors = nodeErrors ?? {}
  }

  /** 把服务端的报错翻译成能看懂的话。 */
  humanSummary() {
    const ids = Object.keys(this.nodeErrors)
    if (!ids.length) return this.message
    const lines = []
    for (const nodeId of ids) {
      const err = this.nodeErrors[nodeId] ?? {}
      const cls = err.class_type ?? '未知节点'
      for (const detail of err.errors ?? []) {
        const extra = detail.extra_info ?? {}
        if (String(detail.message ?? '').includes('not in list')) {
          // 这是最常见的一种：工作流里写死了一个模型/音色文件名，
          // 而这台服务端上没有它。直接说「服务端上没有这个文件」
          // 比原样抛出 not in list 有用得多
          lines.push(
            `节点 ${nodeId}（${cls}）要的 ${extra.input_name ?? ''} 是 ` +
              `${extra.received_value ?? ''}，服务端上没有这个文件`,
          )
        } else {
          lines.push(`节点 ${nodeId}（${cls}）：${detail.message ?? JSON.stringify(detail)}`)
        }
      }
    }
    return lines.length ? lines.join('\n') : this.message
  }
}

/** 执行中失败。可能是偶发的，值得重试。 */
export class ExecutionError extends ComfyError {}

/** 取产出文件。视频节点也把结果放在 images 键下，带 animated 标记。 */
export function jobFiles(result, kind = 'images') {
  const out = []
  for (const nodeOut of Object.values(result.outputs ?? {})) {
    out.push(...(nodeOut[kind] ?? []))
  }
  return out
}

export const firstFile = (result) => jobFiles(result)[0] ?? null

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

export class ComfyClient {
  constructor(
    config,
    {
      clientId = null,
      fetchImpl = fetch,
      WebSocketImpl = WebSocket,
      // 重试之间的退避。做成可注入的，测试里才不用真等几秒——
      // 等待时间长的测试没人愿意跑，没人跑的测试等于没有
      sleepImpl = sleep,
    } = {},
  ) {
    this.config = config
    this.clientId = clientId ?? randomUUID().replace(/-/g, '')
    this.fetch = fetchImpl
    this.WebSocket = WebSocketImpl
    this.sleep = sleepImpl
    this._objectInfo = null
  }

  // ---- 基础 ----

  async _get(pathname) {
    let res
    try {
      res = await this.fetch(`${this.config.base_url}${pathname}`, {
        signal: AbortSignal.timeout(this.config.timeout_s * 1000),
      })
    } catch (err) {
      throw new ComfyUnavailable(
        `连不上 ComfyUI（${this.config.base_url}）。请确认服务已启动且地址正确。\n${err.message}`,
      )
    }
    if (res.status >= 400) {
      throw new ComfyError(`ComfyUI 返回 ${res.status}（${pathname}）`)
    }
    return res.json()
  }

  /** 服务是否可用。 */
  async ping() {
    try {
      await this._get('/system_stats')
      return true
    } catch {
      return false
    }
  }

  systemStats() {
    return this._get('/system_stats')
  }

  /** 节点定义。格式转换和模型清单都要用，缓存起来。 */
  async objectInfo(refresh = false) {
    if (this._objectInfo === null || refresh) {
      this._objectInfo = await this._get('/object_info')
    }
    return this._objectInfo
  }

  /**
   * 查服务端上某个节点的某个下拉框有哪些可选值。
   *
   * 用来在提交之前就发现模型缺失，而不是等服务端拒绝——后者要等到
   * 流水线跑到那一步才炸，而那可能是半小时之后。
   */
  async availableModels(nodeClass, inputName) {
    const info = (await this.objectInfo())[nodeClass]
    if (!info) return []
    for (const section of ['required', 'optional']) {
      const spec = info.input?.[section]?.[inputName]
      if (spec && Array.isArray(spec[0])) return [...spec[0]]
    }
    return []
  }

  // ---- 提交与等待 ----

  /** 提交任务，返回 prompt_id。校验失败会抛 PromptValidationError。 */
  submit(workflow) {
    return this._submitAs(workflow, this.clientId)
  }

  async _submitAs(workflow, clientId) {
    const payload = { prompt: workflow.toJSON ? workflow.toJSON() : workflow, client_id: clientId }
    let res
    try {
      res = await this.fetch(`${this.config.base_url}/prompt`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(payload),
        signal: AbortSignal.timeout(this.config.timeout_s * 1000),
      })
    } catch (err) {
      throw new ComfyUnavailable(`提交任务失败：${err.message}`)
    }

    if (res.status === 400) {
      const text = await res.text()
      let body
      try {
        body = JSON.parse(text)
      } catch {
        throw new PromptValidationError(`提交被拒绝：${text.slice(0, 400)}`)
      }
      throw new PromptValidationError(
        body.error?.message || '工作流校验失败',
        body.node_errors,
      )
    }
    if (res.status >= 400) {
      throw new ComfyError(`提交被拒绝（${res.status}）：${(await res.text()).slice(0, 400)}`)
    }
    return (await res.json()).prompt_id
  }

  /** 等一个任务跑完。走 WebSocket，掉线自动退回轮询。 */
  async wait(promptId, { onProgress = null, timeoutS = null, clientId = null } = {}) {
    const deadline = Date.now() + (timeoutS ?? this.config.job_timeout_s) * 1000
    const url = `${comfyWsUrl(this.config)}?clientId=${clientId ?? this.clientId}`
    let ws
    try {
      ws = await this._openSocket(url)
    } catch {
      // WebSocket 不可用不是致命问题，退回轮询
      return this._waitPoll(promptId, deadline)
    }
    try {
      return await this._consume(ws, promptId, onProgress, deadline)
    } finally {
      try {
        ws.close()
      } catch {
        // 已经关了
      }
    }
  }

  _openSocket(url) {
    return new Promise((resolve, reject) => {
      const ws = new this.WebSocket(url)
      const timer = setTimeout(() => reject(new Error('WebSocket 连接超时')), 10000)
      ws.addEventListener('open', () => {
        clearTimeout(timer)
        resolve(ws)
      })
      ws.addEventListener('error', (event) => {
        clearTimeout(timer)
        reject(new Error(event?.message ?? 'WebSocket 连接失败'))
      })
    })
  }

  /**
   * 消费 WebSocket 消息直到任务结束。
   *
   * 收不到消息时每 5 秒查一次历史兜底——一段时间没消息不代表任务还在跑。
   * 完成消息可能根本没送到：任务在 WebSocket 连上之前就结束了，
   * 或者同一个 clientId 上有并发任务、后连的把先连的挤掉了。
   * 不兜底的话这里会白等到 job_timeout_s，默认是半小时。
   */
  async _consume(ws, promptId, onProgress, deadline) {
    const start = Date.now()
    const queue = []
    let waiter = null
    let closed = null

    const push = (item) => {
      if (waiter) {
        const w = waiter
        waiter = null
        w(item)
      } else {
        queue.push(item)
      }
    }
    ws.addEventListener('message', (e) => push({ data: e.data }))
    ws.addEventListener('close', () => {
      closed = new Error('WebSocket 断开')
      push(null)
    })
    ws.addEventListener('error', () => {
      closed = new Error('WebSocket 出错')
      push(null)
    })

    const next = (ms) =>
      new Promise((resolve) => {
        if (queue.length) return resolve(queue.shift())
        const timer = setTimeout(() => {
          waiter = null
          resolve(undefined) // 静默，去查历史
        }, ms)
        waiter = (item) => {
          clearTimeout(timer)
          resolve(item)
        }
        return undefined
      })

    for (;;) {
      const remaining = deadline - Date.now()
      if (remaining <= 0) throw new ExecutionError(`任务 ${promptId} 超时`)

      const item = await next(Math.min(remaining, RECHECK_EVERY_MS))

      if (item === undefined || item === null) {
        // 静默或断开：查一次历史兜底
        const outputs = await this._historyOutputs(promptId, true)
        if (outputs !== null) {
          return { prompt_id: promptId, outputs, elapsed_s: (Date.now() - start) / 1000 }
        }
        if (closed) {
          // 连接没了，剩下的路只能靠轮询
          return this._waitPoll(promptId, deadline)
        }
        continue
      }

      // 二进制是预览图，忽略
      if (typeof item.data !== 'string') continue

      let msg
      try {
        msg = JSON.parse(item.data)
      } catch {
        continue
      }
      const { type, data = {} } = msg
      if (data.prompt_id !== undefined && data.prompt_id !== null && data.prompt_id !== promptId) {
        continue
      }

      if (type === 'progress' && onProgress) {
        const step = data.value ?? 0
        const total = data.max ?? 0
        onProgress({
          prompt_id: promptId,
          node_id: data.node ?? null,
          step,
          total,
          fraction: total ? step / total : 0,
        })
      } else if (type === 'execution_error') {
        throw new ExecutionError(
          `节点 ${data.node_type} 执行失败：${data.exception_message}`,
        )
      } else if (type === 'execution_interrupted') {
        throw new ExecutionError(`任务 ${promptId} 被中断`)
      } else if (type === 'executing' && (data.node === null || data.node === undefined)) {
        // node 为 null 表示整个任务跑完
        const outputs = await this._historyOutputs(promptId)
        return { prompt_id: promptId, outputs, elapsed_s: (Date.now() - start) / 1000 }
      }
    }
  }

  async _waitPoll(promptId, deadline) {
    const start = Date.now()
    while (Date.now() < deadline) {
      const hist = await this._get(`/history/${promptId}`)
      const entry = hist[promptId]
      if (entry) {
        const status = entry.status ?? {}
        if (status.status_str === 'error') {
          throw new ExecutionError(errorFromHistory(status))
        }
        return {
          prompt_id: promptId,
          outputs: entry.outputs ?? {},
          elapsed_s: (Date.now() - start) / 1000,
        }
      }
      await this.sleep(2000)
    }
    throw new ExecutionError(`任务 ${promptId} 超时`)
  }

  /**
   * 从历史里取产出。
   *
   * missingOk 表示这是一次兜底查询：任务还没跑完就返回 null，
   * 让调用方接着等，而不是把空结果当成跑完了。
   */
  async _historyOutputs(promptId, missingOk = false) {
    const hist = await this._get(`/history/${promptId}`)
    const entry = hist[promptId]
    if (entry === undefined) return missingOk ? null : {}
    const status = entry.status ?? {}
    if (status.status_str === 'error') throw new ExecutionError(errorFromHistory(status))
    if (missingOk && status.completed === false) return null
    return entry.outputs ?? {}
  }

  /** 提交并等待完成。执行类错误会按配置重试，校验类错误直接抛。 */
  async run(workflow, { onProgress = null, timeoutS = null } = {}) {
    let last = null
    for (let attempt = 0; attempt <= this.config.max_retries; attempt += 1) {
      try {
        // 每个任务用独立的 clientId。ComfyUI 按 clientId 记订阅，
        // 同一个 id 上并发跑两个任务，后连的会把先连的挤下线，
        // 先连的那个永远等不到完成消息，白等到超时为止。
        const jobId = `${this.clientId}-${randomUUID().slice(0, 8)}`
        const promptId = await this._submitAs(workflow, jobId)
        return await this.wait(promptId, { onProgress, timeoutS, clientId: jobId })
      } catch (err) {
        if (err instanceof PromptValidationError) throw err // 重试也不会变好
        if (!(err instanceof ExecutionError) && !(err instanceof ComfyUnavailable)) throw err
        last = err
        if (attempt < this.config.max_retries) await this.sleep(2 ** attempt * 1000)
      }
    }
    throw new ExecutionError(`重试 ${this.config.max_retries} 次后仍失败：${last?.message}`)
  }

  // ---- 文件 ----

  /**
   * 上传图片到服务端的 input 目录，返回可在工作流里引用的文件名。
   *
   * ComfyUI 可能在另一台机器上，所以不能直接传本地路径。
   */
  async uploadImage(filePath, subfolder = '') {
    if (!fs.existsSync(filePath) || !fs.statSync(filePath).isFile()) {
      throw new ComfyError(`要上传的文件不存在：${filePath}`)
    }
    const form = new FormData()
    form.append('image', new Blob([fs.readFileSync(filePath)]), path.basename(filePath))
    form.append('overwrite', 'true')
    if (subfolder) form.append('subfolder', subfolder)

    const res = await this.fetch(`${this.config.base_url}/upload/image`, {
      method: 'POST',
      body: form,
      signal: AbortSignal.timeout(this.config.timeout_s * 1000),
    })
    if (res.status >= 400) {
      throw new ComfyError(`上传失败（${res.status}）：${(await res.text()).slice(0, 300)}`)
    }
    const body = await res.json()
    const sub = body.subfolder || ''
    return sub ? `${sub}/${body.name}` : body.name
  }

  /** 把产出文件下载到本地。同样因为服务端可能在别的机器上。 */
  async download(fileRef, dest) {
    const params = new URLSearchParams({
      filename: fileRef.filename,
      subfolder: fileRef.subfolder ?? '',
      type: fileRef.type ?? 'output',
    })
    fs.mkdirSync(path.dirname(dest), { recursive: true })
    const res = await this.fetch(`${this.config.base_url}/view?${params}`, {
      signal: AbortSignal.timeout(this.config.job_timeout_s * 1000),
    })
    if (res.status >= 400) {
      throw new ComfyError(`下载失败（${res.status}）：${fileRef.filename}`)
    }
    await pipeline(Readable.fromWeb(res.body), fs.createWriteStream(dest))
    return dest
  }

  async interrupt() {
    await this.fetch(`${this.config.base_url}/interrupt`, {
      method: 'POST',
      signal: AbortSignal.timeout(this.config.timeout_s * 1000),
    })
  }
}

function errorFromHistory(status) {
  for (const msg of status.messages ?? []) {
    if (Array.isArray(msg) && msg.length >= 2 && msg[0] === 'execution_error') {
      const detail = msg[1] ?? {}
      return `节点 ${detail.node_type} 执行失败：${detail.exception_message}`
    }
  }
  return '任务执行失败'
}
