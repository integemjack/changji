/**
 * 异步动作的三件套：忙不忙、报错了没、跑完刷不刷进度。
 *
 * 每个页面都要写一遍 try/catch/finally + 弹提示 + 刷新，写十遍就漏一遍。
 * 收在这里，页面里只剩一句 run(...)。
 */

import { computed, reactive, ref } from 'vue'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

export function useAction() {
  const ui = useUi()
  const session = useSession()
  /**
   * 正在跑的那些动作，按 key 记。
   *
   * ⚠️ **以前这儿是一个 `busy` 布尔，而且 run() 开头一句
   * `if (busy.value) return undefined`——于是整页同时只能干一件事。**
   *
   * 2026-09-11 用户报的就是它：设定页上点了一个「画」，再点另一个位置的
   * 「画」，**什么都不会发生**——没反应、不报错、按钮也不变灰，看着就像
   * 界面坏了。而 AssetCharacters 里那段注释还写着"画着的那张只停自己那一
   * 格，另外两格照样能点"，因为 `isBusy(key)` 确实是按 key 分的——分的只是
   * "哪个按钮显示成忙"，真正拦人的那一句不认 key。
   *
   * 以前不明显，是因为出图是同步的：那几十秒里整页本来就动不了。改成
   * 异步之后界面是活的，这一下就暴露成"点了没用"。
   *
   * 现在按 key 各跑各的；同一个 key 再点还是拦（防手抖连点）。
   * 不给 key 的那些共用空串这一个格子，行为和以前一样。
   */
  const running = reactive(new Set())
  const busy = computed(() => running.size > 0)
  /** 最近一次开工的那个 key。留着是为了不改调用方，新代码别依赖它。 */
  const busyKey = ref('')
  const error = ref('')

  /**
   * @param {Function} fn 真正干活的函数
   * @param {object} opts key 用来区分同页多个按钮；success 成功提示；
   *                     refresh 跑完是否重算流程进度
   */
  async function run(fn, { key = '', success = '', refresh = false, quiet = false } = {}) {
    if (running.has(key)) return undefined
    running.add(key)
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
      running.delete(key)
      if (busyKey.value === key) busyKey.value = ''
    }
  }

  /** 这个 key 在跑吗。不给 key 就是"有没有任何东西在跑"。 */
  const isBusy = (key) => (key ? running.has(key) : running.size > 0)

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

/**
 * 字节数转成「18.8 GB」。初始化页整页都靠它。
 *
 * **用 1000 进制不是 1024。** 模型页上的数字要和用户在硬盘属性、
 * 云盘、HuggingFace 页面上看到的对得上——那些地方一律是 GB（1000³）。
 * 换成 GiB 的话，同一个文件这里写 17.5、别处写 18.8，
 * 用户会以为下漏了一块。
 */
export function humanBytes(bytes) {
  const n = Number(bytes) || 0
  if (n <= 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let value = n
  let at = 0
  while (value >= 1000 && at < units.length - 1) {
    value /= 1000
    at += 1
  }
  // 字节和 KB 不带小数：「512 B」比「512.0 B」好读
  const digits = at <= 1 ? 0 : value >= 100 ? 0 : 1
  return `${value.toFixed(digits)} ${units[at]}`
}

/** 每秒多少字节转成「12.4 MB/s」。0 或者算不出来时返回空串。 */
export function humanRate(bytesPerSecond) {
  const n = Number(bytesPerSecond) || 0
  if (n <= 0) return ''
  return `${humanBytes(n)}/s`
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
