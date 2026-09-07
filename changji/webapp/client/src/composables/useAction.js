/**
 * 异步动作的三件套：忙不忙、报错了没、跑完刷不刷进度。
 *
 * 每个页面都要写一遍 try/catch/finally + 弹提示 + 刷新，写十遍就漏一遍。
 * 收在这里，页面里只剩一句 run(...)。
 */

import { ref } from 'vue'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

export function useAction() {
  const ui = useUi()
  const session = useSession()
  const busy = ref(false)
  const busyKey = ref('')
  const error = ref('')

  /**
   * @param {Function} fn 真正干活的函数
   * @param {object} opts key 用来区分同页多个按钮；success 成功提示；
   *                     refresh 跑完是否重算流程进度
   */
  async function run(fn, { key = '', success = '', refresh = false, quiet = false } = {}) {
    if (busy.value) return undefined
    busy.value = true
    busyKey.value = key
    error.value = ''
    try {
      const result = await fn()
      if (success) ui.ok(success)
      if (refresh) await session.refresh()
      return result
    } catch (err) {
      error.value = err.message || String(err)
      if (!quiet) ui.error(error.value)
      return undefined
    } finally {
      busy.value = false
      busyKey.value = ''
    }
  }

  const isBusy = (key) => busy.value && (!key || busyKey.value === key)

  return { busy, busyKey, error, run, isBusy }
}

/** 秒转成「3 分 20 秒」。界面上到处要用。 */
export function humanTime(seconds) {
  const s = Math.round(Number(seconds) || 0)
  if (s < 60) return `${s} 秒`
  const m = Math.floor(s / 60)
  const rest = s % 60
  if (m < 60) return rest ? `${m} 分 ${rest} 秒` : `${m} 分`
  const h = Math.floor(m / 60)
  return `${h} 小时 ${m % 60} 分`
}

/** 时间戳转成「3 分钟前」。项目列表和投递记录用。 */
export function humanAgo(input) {
  const t = typeof input === 'number' ? input * 1000 : Date.parse(input)
  if (!t || Number.isNaN(t)) return ''
  const diff = (Date.now() - t) / 1000
  if (diff < 60) return '刚刚'
  if (diff < 3600) return `${Math.floor(diff / 60)} 分钟前`
  if (diff < 86400) return `${Math.floor(diff / 3600)} 小时前`
  if (diff < 86400 * 30) return `${Math.floor(diff / 86400)} 天前`
  return new Date(t).toLocaleDateString('zh-CN')
}
