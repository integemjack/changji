/**
 * 大模型客户端。
 *
 * 只走 OpenAI 兼容接口——各家云服务和本地推理框架都提供它，所以不用为
 * 每一家写适配器，填对地址和密钥就能用。
 *
 * 两件事是这一层的全部价值：
 *
 * 一，约束解码。schema 交给服务端，模型就编不出不存在的角色 id、
 *     填不了外观字段、也写不出档位表之外的时长。不支持 json_schema 的
 *     服务退回宽松的 json_object，再由调用方自己校验。
 * 二，把失败翻成人话。地址错、模型没拉、密钥过期是最常见的三类，
 *     原样抛 HTTP 错误的话用户只看到一个 404，不知道该去改什么。
 */

export class LLMError extends Error {}

/**
 * 把 HTTP 状态码翻成一句能照着做的话。
 *
 * 最要紧的一条：Ollama 在「地址不对」和「模型没拉下来」两种情况下都回 404。
 * 一律说「地址填错了」会把人支到错误的地方去查，比不给提示更费时间，
 * 所以看服务端自己的话来分。
 */
export function explainHttpError(config, status, body) {
  const url = `${config.base_url}/chat/completions`
  let detail = ''
  try {
    const parsed = typeof body === 'string' ? JSON.parse(body) : body
    const inner = parsed?.error ?? parsed?.message ?? ''
    detail = String(typeof inner === 'object' ? JSON.stringify(inner) : inner).slice(0, 200)
  } catch {
    detail = String(body ?? '').slice(0, 200)
  }

  let hint
  if (status === 404) {
    const low = detail.toLowerCase()
    if (low.includes('model') && (low.includes('not found') || low.includes('not exist'))) {
      hint =
        `大模型服务在，但它没有 ${config.model} 这个模型。` +
        `本地 Ollama 的话先 ollama pull ${config.model}，` +
        `或者去设置页的「大模型」那一节换一个已经有的模型名。`
    } else {
      hint =
        `大模型服务在 ${url} 上没有这个接口。多半是地址填错了——` +
        `地址要带 /v1 结尾，而且那台机器上的服务得真的起着。` +
        `去设置页的「大模型」那一节改。`
    }
  } else if (status === 401 || status === 403) {
    hint = `大模型服务拒绝了这次请求（${status}），八成是 API Key 不对。去设置页改。`
  } else if (status === 400 && /model/i.test(detail)) {
    // Ollama 把「名字写得不合法」和「模型没拉下来」分成 400 和 404 两种。
    // 前者常见于手打模型名时多打了空格或中文
    hint =
      `大模型服务不认 ${config.model} 这个模型名。检查有没有多余的空格或` +
      `打错字，或者去设置页从下拉列表里挑一个。`
  } else if (status === 429) {
    hint = '大模型服务说请求太频繁了，等一会儿再试。'
  } else if (status >= 500) {
    hint =
      `大模型服务自己出错了（${status}）。本地服务的话看一眼它的日志，` +
      `云服务的话过一会儿再试。`
  } else {
    hint = `大模型服务返回 ${status}。`
  }

  const model = config.model ? `\n当前模型：${config.model}` : ''
  return hint + model + (detail ? `\n服务说：${detail}` : '')
}

/**
 * 从大模型输出里抠出 JSON。
 *
 * 即使要求只输出 JSON，模型也常包一层代码块或在前面加一句话。
 */
export function extractJson(raw) {
  let text = String(raw).trim()
  const fence = text.match(/```(?:json)?\s*([\s\S]+?)```/)
  if (fence) text = fence[1].trim()

  try {
    return JSON.parse(text)
  } catch {
    // 往下退一步
  }

  // 找第一个平衡的对象或数组
  for (const [opener, closer] of [['{', '}'], ['[', ']']]) {
    const start = text.indexOf(opener)
    if (start === -1) continue
    let depth = 0
    for (let i = start; i < text.length; i += 1) {
      if (text[i] === opener) depth += 1
      else if (text[i] === closer) {
        depth -= 1
        if (depth === 0) {
          try {
            return JSON.parse(text.slice(start, i + 1))
          } catch {
            break
          }
        }
      }
    }
  }
  throw new LLMError(`大模型输出里找不到合法 JSON：\n${String(raw).slice(0, 400)}`)
}

/**
 * 发一次补全，返回模型输出的文本。
 *
 * schema 给了就用 json_schema 约束解码；服务端不认（返回 4xx）就退回
 * json_object 再试一次。有些自建服务和老版本只认后者。
 */
export async function complete(config, prompt, { schema, name = 'result', fetchImpl = fetch } = {}) {
  const payload = {
    model: config.model,
    messages: [{ role: 'user', content: prompt }],
    temperature: config.temperature,
  }
  if (schema) {
    payload.response_format = {
      type: 'json_schema',
      json_schema: { name, strict: true, schema },
    }
  }

  const url = `${config.base_url}/chat/completions`
  const headers = {
    'content-type': 'application/json',
    authorization: `Bearer ${config.api_key}`,
  }

  const post = async (body) => {
    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), config.timeout_s * 1000)
    try {
      return await fetchImpl(url, {
        method: 'POST',
        headers,
        body: JSON.stringify(body),
        signal: controller.signal,
      })
    } finally {
      clearTimeout(timer)
    }
  }

  let res
  try {
    res = await post(payload)
  } catch (err) {
    if (err.name === 'AbortError' || err.name === 'TimeoutError') {
      throw new LLMError(
        `等大模型超过 ${config.timeout_s} 秒还没回话（${config.base_url}）。` +
          `模型太大或者机器太忙，可以去设置页把超时调长。`,
      )
    }
    throw new LLMError(`连不上大模型服务（${config.base_url}）。\n${err.message}`)
  }

  /*
   * 有些服务不支持 json_schema，退回宽松的 json_object 再试一次。
   *
   * 只在 400 / 422 上退，不是「任何 4xx 都退」。
   *
   * 踩过：模型名写错时 Ollama 回 404「model not found」，而带着
   * json_object 重试一次会撞上另一个 400，最后报出来的是那个 400——
   * 一句「大模型服务返回 400」，把原本清清楚楚的「这个模型没拉下来」
   * 盖掉了。404 说明地址或模型不对，换个 response_format 再问一遍
   * 不可能变好，只会毁掉唯一有用的那条线索。
   *
   * 失败时报第一次的错：它更接近真正的原因。
   */
  if (schema && (res.status === 400 || res.status === 422)) {
    const first = { status: res.status, body: await res.text() }
    res = await post({ ...payload, response_format: { type: 'json_object' } })
    if (res.status >= 400) {
      throw new LLMError(explainHttpError(config, first.status, first.body))
    }
  }

  if (res.status >= 400) {
    throw new LLMError(explainHttpError(config, res.status, await res.text()))
  }

  let body
  try {
    body = await res.json()
  } catch {
    throw new LLMError(`大模型返回的不是 JSON（${url}）`)
  }
  const content = body?.choices?.[0]?.message?.content
  if (typeof content !== 'string') {
    throw new LLMError(`大模型返回格式异常：${JSON.stringify(body).slice(0, 300)}`)
  }
  return content
}

/** 发一次补全并把输出解析成 JSON。 */
export async function completeJson(config, prompt, opts = {}) {
  return extractJson(await complete(config, prompt, opts))
}

/**
 * 那台服务上有哪些模型。
 *
 * 模型名以前只能手打，打错了要跑到写剧本那一步才报错，而报出来的 404
 * 分不清是地址错了还是名字错了。拉过来给人选，这类错就没机会发生。
 * 问不到就返回空列表并说明原因，界面退回手打，不至于因为列不出来就没法填。
 */
export async function listModels(config, { fetchImpl = fetch, timeoutMs = 10000 } = {}) {
  const url = `${config.base_url}/models`
  let res
  try {
    res = await fetchImpl(url, {
      headers: { authorization: `Bearer ${config.api_key}` },
      signal: AbortSignal.timeout(timeoutMs),
    })
  } catch (err) {
    return { models: [], error: `连不上 ${url}：${err.message}` }
  }
  if (res.status >= 400) return { models: [], error: `${url} 返回 ${res.status}` }

  let body
  try {
    body = await res.json()
  } catch {
    return { models: [], error: `${url} 返回的不是 JSON` }
  }
  const items = Array.isArray(body) ? body : (body?.data ?? [])
  const names = []
  for (const item of items) {
    const name = typeof item === 'string' ? item : item?.id
    if (name) names.push(String(name))
  }
  return { models: [...new Set(names)].sort(), current: config.model }
}
