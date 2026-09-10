/**
 * 第一次运行时先去初始化页。
 *
 * **判据在引擎那边，不在浏览器里。** "模型配齐了没有"是这台机器的事实
 * （文件在不在盘上、配置指没指对），只有引擎知道。前端存一个"我装过了"
 * 的标记是不行的：换台设备打开、清一次浏览器数据、或者模型被挪走了，
 * 标记都还在，而用户会进到一个什么都跑不了的首页。
 *
 * 浏览器这边只存一件事：**用户在那一页上做过决定了**。那是个主观事实，
 * 引擎无从知道，也不该写进它的配置——模型缺不缺是客观事实，不能因为
 * 有人点了一次「先跳过」就当它配好了。
 *
 * 「做过决定」包括两种，两种都算：
 *   点了「先跳过」；
 *   点了下载并且跑完了（哪怕其中几组他故意选的是「不下载」）。
 * 第二种不算的话，故意只下出图那一套的人**每次打开都会被拦一遍**，
 * 而他每次都要再点一次「先跳过」。
 *
 * ---
 *
 * 反过来还有一条：**正在下的时候一律拦回去**，跳过标记也不管用。
 * 43 GB 要下几个小时，这期间用户多半会关掉浏览器。第二天打开时最该看到的
 * 就是那条进度——而不是一个什么都跑不了的首页加一个"还缺模型"的红字。
 *
 * ---
 *
 * **一个会话只问一次。** 每次切页面都去问的话，八步来回走一遍就是十几个
 * 请求，而这个判断在一次会话里几乎不会变。真变了（在初始化页上下完了）
 * 的那一处会自己把标记清掉，见 clearSetupCheck。
 */

import { api } from '@/api'

const HANDLED_KEY = 'changji.setup.handled'

/** 这一次会话里问过没有。undefined = 还没问。 */
let checked

/** 用户在初始化页上做过决定了，别再拦他。 */
export function markSetupHandled() {
  try {
    localStorage.setItem(HANDLED_KEY, '1')
  } catch {
    // 隐私模式下 localStorage 会抛。下次打开会再问一遍——比整个界面
    // 卡在这里好。
  }
  // **是作废，不是直接置成"不用拦"。** 置死的话"正在下就拦回去"那一条
  // 会被这里短路掉：下完一轮标了这一笔，紧接着又开了一轮下载，
  // 而守卫拿着缓存的 false 把人放去了首页，进度就没人看得见了。
  checked = undefined
}

export function setupHandled() {
  try {
    return localStorage.getItem(HANDLED_KEY) === '1'
  } catch {
    return false
  }
}

/** 让下一次导航重新问一遍引擎。下完之后调它。 */
export function clearSetupCheck() {
  checked = undefined
}

/**
 * 现在该不该把人送去初始化页。
 *
 * 问不到引擎一律返回 false。**这一条是有意的**：引擎没起来、网络断了、
 * 接口 404（跑在老的 Node 层后面）都会走到这里，而那时候把人拦在
 * 初始化页上只会让他更不知道发生了什么——那一页自己也读不到清单。
 */
/**
 * 问引擎的那一下**必须有个上限**。
 *
 * 路由守卫是 await 它的，也就是说这个 Promise 不落地，界面上一个像素都
 * 不会画——用户看到的就是白屏。而 /bff/setup/state 那边要向两个下载源
 * 各发一次 HEAD，实测 3.5 到 4 秒，网络差的时候两个各等 6 秒超时。
 * 用户报的"刷新页面白屏"就是这么来的：不是崩了，是守卫还在等。
 *
 * 引擎那边已经把探测结果缓存住了，但**前端不能依赖那个**：老版本的引擎、
 * 反向代理卡住、连接被中间设备吊着不放，都会让这一下无限期挂着。
 * 超时了就当"问不到"——那条路本来就是放行，见上面的注释。
 */
const ASK_TIMEOUT_MS = 1500

function withTimeout(p, ms) {
  return Promise.race([
    p,
    new Promise((_, reject) =>
      setTimeout(() => reject(new Error('问引擎超时')), ms)),
  ])
}

export async function shouldSetup() {
  if (checked !== undefined) return checked
  try {
    const state = await withTimeout(api.setupState(), ASK_TIMEOUT_MS)
    // 正在下就一定回去看进度，跳过标记不管用。
    if (state?.download?.state === 'running') {
      checked = true
      return true
    }
    checked = !setupHandled() && Boolean(state?.needed)
  } catch (err) {
    // **超时不记账。** 记成 false 的话这一整个会话都不会再问，
    // 而超时多半是这一下慢，不是"不需要初始化"。下次导航再问一遍，
    // 那时引擎那边的缓存多半已经热了，几毫秒就回来。
    if (String(err?.message || '').includes('超时')) return false
    checked = false
    return checked
  }
  return checked
}
