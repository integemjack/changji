/**
 * 换版之后懒加载的页面拿不到了——判据和"要不要自动重载一次"。
 *
 * **单独一个文件，是为了能被测到。** 放在 router/index.js 里的话，
 * 测它就得起一整个真路由；而这两件事本身是纯逻辑，值得单独盯住：
 * 判据宽了会把普通错误也变成整页重载，窄了就还是白屏。
 */

/**
 * 这个错是不是"动态 import 的资源没拿到"。
 *
 * 各家浏览器措辞不一样，四条都要认。**只认这几条**：判宽了的话，
 * 一个普通的接口错误会触发整页重载，用户正填着的东西就没了。
 */
export function isChunkLoadError(err) {
  const msg = String(err?.message || err || '')
  return (
    msg.includes('Failed to fetch dynamically imported module') || // Chrome / Edge
    msg.includes('error loading dynamically imported module') ||   // Firefox
    msg.includes('Importing a module script failed') ||            // Safari
    msg.includes('Unable to preload CSS')                          // Vite 预加载 CSS
  )
}

export const RELOADED_KEY = 'changji.chunk-reloaded'

/**
 * 这一次该不该自动整页重载。
 *
 * **一次会话只重载一次。** 真的是资源没了、重载也拿不回来的时候，
 * 不设这道闸就会一直刷，用户连那句错误提示都看不到。
 *
 * `storage` 传进来而不是直接用 sessionStorage：隐私模式下它会抛，
 * 测试里也没有。抛了就当"没重载过"——最坏多刷一次，比停在白屏上强。
 */
export function shouldAutoReload(storage) {
  try {
    if (storage?.getItem(RELOADED_KEY) === '1') return false
    storage?.setItem(RELOADED_KEY, '1')
    return true
  } catch {
    return true
  }
}

/** 导航成功了，把那一笔清掉。不清的话这一会话里下次换版就不会自动重载。 */
export function clearReloadMark(storage) {
  try {
    storage?.removeItem(RELOADED_KEY)
  } catch {
    // 隐私模式，无所谓
  }
}
