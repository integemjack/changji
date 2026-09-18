/**
 * 读写浏览器上那两份存储，**碰上不给用的浏览器就当没有**。
 *
 * ⚠️ **`localStorage` 不是"读不到就回 null"那么温和：它会在属性访问那一下
 * 直接抛。** Chrome 把 cookie 设成「全部阻止」时，`window.localStorage` 本身
 * 就是一个 SecurityError；Firefox 关掉 dom.storage 之后它是 undefined；
 * Safari 无痕窗口里早年的行为是 setItem 抛配额。这几种都不需要用户"懂技术"
 * 才会碰上，随手在设置里点一下就有了。
 *
 * 抛在哪儿决定了后果有多大。这套代码里读存储最早的两处是
 * `stores/session.js` 和 `stores/ui.js` 的 **setup 顶上**——而 `App.vue`
 * 的 setup 第一件事就是 `useSession()` / `useUi()`。也就是说它抛的位置在
 * **整个应用挂载之前、ErrorBoundary 之外**：App 装不上，`#app` 里一个节点
 * 都没有，`ToastStack` 自然也没有，`main.js` 那个 errorHandler 发出来的
 * `changji:error` 没人订。表现就是一片白，控制台一行，界面上一个字都没有
 * ——router/index.js 里那段话原样适用：「不接的话页面全白、控制台一行、
 * 界面上一个字都没有」。
 *
 * 更隐蔽的一处是 `router/index.js` 的 `afterEach`：它每一次导航都跑，里面
 * 那句 `globalThis.sessionStorage` 一抛，**每一次跳页都在抛**，而它抛出来
 * 又会被 `router.onError` 接住、那儿再访问一次同一个属性。
 *
 * 这几件事都不该发生：记的东西全是**方便**——上次开的哪部电影、栏靠哪边、
 * 左栏收没收起。一个都拿不到的代价只是"每次打开都是默认值"，而现在的
 * 代价是整个应用打不开。
 *
 * 所以这一层：**存储拿不到就当它是空的，读回 null、写进黑洞。**
 * 和 `router/chunk-error.js` 那两个函数是同一个规矩（那儿的注释写着
 * 「隐私模式下它会抛」「抛了就当"没重载过"」），只是那儿把 storage 当参数
 * 传进去，而这儿要的是"连拿都拿不到"也不炸。
 *
 * 单独一个文件、不依赖 Vue，理由同 chunk-error.js：**这样才测得到**。
 */

/**
 * 拿到那个 storage，拿不到就 null。
 *
 * 每次都现拿而不是在模块顶上取一次存着：模块顶上那一下同样会抛，而且那是
 * import 期间，比 setup 更早、更没得救。
 *
 * @param {'local'|'session'} which
 * @returns {Storage|null}
 */
export function store(which) {
  try {
    const s = which === 'session' ? globalThis.sessionStorage : globalThis.localStorage
    // 存在但不是 Storage（有些扩展会塞个假的）也当没有
    return s && typeof s.getItem === 'function' ? s : null
  } catch {
    return null
  }
}

/** 这台机器上记的那个值。拿不到（不给用 / 没存过）一律 null，和 getItem 一样。 */
export function readLocal(key) {
  try {
    return store('local')?.getItem(key) ?? null
  } catch {
    // getItem 自己也可能抛（个别浏览器的降级实现）
    return null
  }
}

/** 记下来。记不住不是错——下次打开是默认值而已。 */
export function writeLocal(key, value) {
  try {
    store('local')?.setItem(key, value)
  } catch {
    // 配额满了、无痕窗口、不给用。都不值得打扰用户。
  }
}

/** 忘掉它。 */
export function dropLocal(key) {
  try {
    store('local')?.removeItem(key)
  } catch {
    // 同上
  }
}
