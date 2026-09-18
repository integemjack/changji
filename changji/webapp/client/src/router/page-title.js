/**
 * 标签页上那个名字。
 *
 * **被拦下来的那一次不能改它。** `router.afterEach` 连没走成的导航也会
 * 走到（第三个参数就是那次失败），而那时候人还在原来那一页——照着 `to`
 * 写的话，标签页上写着「故事」而屏幕上是「这一章」，切标签页、存书签、
 * 翻历史记录全都跟着错。
 *
 * 拦得下来的地方现在有两处，都是"走了就没了"的东西：剧本页写着的或者
 * 还没采用的那一篇、设定页剪好还没存的那条预告片。它们在
 * `onBeforeRouteLeave` 里问一句「确定？」，人点取消就是这条路。
 *
 * 单独一个文件是为了能测：`afterEach` 要有个真 router 才跑得起来，
 * 而 `router/index.js` 一进来就 `createWebHistory()`。chunk-error 那几个
 * 函数分出来也是这个理由。
 */

/**
 * 拼出这一页该显示的名字。
 *
 * **和导航栏同源**：两边都直接用 `meta.title`（路由表里那个字面量），
 * 中间不再有一层翻译。2026-09-16 一度是「导航栏过 chapterWord、标签页
 * 不过」，于是同一页屏幕左上写「这一章」、标签页写「这一集 · 场记」；
 * 补上那一层之后又撞到第二种飘法——那一层要发请求问"章模式开没开"，
 * 引擎重启那几秒请求被拒就兜底成老叫法。
 *
 * 集模式当天整个删了，翻译层跟着没了：名字在路由表里写成什么就是什么。
 */
export function titleOf(to) {
  return to?.meta?.title ? `${to.meta.title} · 场记` : '场记'
}

/**
 * @param {object} to        要去的那一页
 * @param {object} [failure] 这一次导航的失败（`afterEach` 的第三个参数）
 * @param {object} [doc]     写到哪儿。留个口子只为了测。
 */
export function applyTitle(to, failure, doc = globalThis.document) {
  if (failure) return
  if (doc) doc.title = titleOf(to)
}
