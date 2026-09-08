/**
 * 订阅引擎的任务进度。
 *
 * 两条线共用：流水线（"run"）和写整季 / 批量分镜（"write"）。
 * 抽出来是因为两边要处理的是同一串事：连、订、收、断了要让调用方知道。
 * 各写一份的话迟早分叉，而分叉的表现是"其中一条线的进度不动了"，
 * 很难联想到是两份拷贝没同步。
 *
 * ⚠️ **按任务类别订阅，不是按 job_id。** 引擎的 job_id 是随机生成的，
 * 而且不从任何接口暴露出去（`POST /api/run` 回 {started, queue}，
 * `GET /api/run` 那些字段里也没有）。按类别订阅还有一个好处：
 * **可以在任务开始之前订上**，中间不会漏消息。
 */

/** 引擎的任务类别。和 job_id 的前缀对应（"run-a3f..." → "run"）。 */
export const JOB_KINDS = ['run', 'write']

export function jobSocketUrl() {
  const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${proto}//${window.location.host}/api/ws`
}

/**
 * 连上并订阅一类任务。
 *
 * @param {string} kind      "run" 或 "write"
 * @param {(msg: object) => void} onMessage 收到一条消息（已经解析成对象）
 * @param {() => void} [onDrop] 断开时叫一声，调用方据此退回轮询
 * @returns {{ close: () => void }} 关掉它；重复关是安全的
 */
export function openJobSocket(kind, onMessage, onDrop) {
  let sock
  try {
    sock = new WebSocket(jobSocketUrl())
  } catch {
    // 连不上不是错误，是"这条路没有"。调用方的轮询一直开着。
    onDrop?.()
    return { close() {} }
  }

  sock.onopen = () => {
    sock.send(JSON.stringify({ type: 'subscribe', job_id: kind }))
  }
  sock.onmessage = (ev) => {
    let msg
    try {
      msg = JSON.parse(ev.data)
    } catch {
      return // 一条坏消息不该掀翻这一屏
    }
    onMessage(msg)
  }
  // onclose 和 onerror 都可能先到，谁先到算谁的，但只叫一次
  let dropped = false
  const drop = () => {
    if (dropped) return
    dropped = true
    onDrop?.()
  }
  sock.onclose = drop
  sock.onerror = drop

  return {
    close() {
      // 主动关的时候不叫 onDrop——那是给"意外断开"用的，
      // 调用方自己关的时候它已经知道了。
      dropped = true
      sock.onclose = null
      sock.onerror = null
      try {
        sock.close()
      } catch {
        // 还在 CONNECTING 时 close 在个别浏览器上会抛，忽略
      }
    },
  }
}
