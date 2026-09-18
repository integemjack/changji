/**
 * 「故事」这一步做完了没有——**判据只该有一处**。
 *
 * 单独一个文件，理由和 `router/chunk-error.js` 一样：为了能被测到。
 * 它本身是纯逻辑，而错了的表现不报错、只是导航上少一格，很难看出来。
 *
 * ---- 它必须和引擎逐字一致 ----
 *
 * 顶栏上「设定」那一格是按 `session.done.story` 显示的（App.vue 的
 * visibleSteps），而那个值是引擎在 `flow.cpp` 里算的：
 *
 *     done["story"] = any_of(chapters): !summary.empty() || !text.empty()
 *
 * 这边照抄。两处各写一份判据的下场，`capability.hpp` 里那段说透了：
 * 「迟早出现『表上说能干，派过去却被拒』」。这儿对应的是"故事页觉得
 * 写完了、导航觉得没有"，然后来回问引擎。
 *
 * ⚠️ **不许 trim。** 引擎判的是原串非空。这边一 trim，一个只敲了空格的
 * 章就变成"两边永远对不上"——而 `needsFlowReread` 正是拿这两个值比的，
 * 对不上就等于故事每变一次白问一趟 `/bff/flow`（全站最慢的那条）。
 *
 * 为什么不能只看"有没有章节"，`flow.cpp` 那段注释写着：「直接开写」建的
 * 是**一章空的**，按章节数判的话它一按下去这一步就打上勾。
 */
export function storyIsDone(chapters) {
  return (chapters ?? []).some(
    (c) => (c?.summary ?? '') !== '' || (c?.text ?? '') !== '',
  )
}

/**
 * 这会儿该不该重读一趟流程（`session.refresh()`）。
 *
 * **只在两边不一致时才问。** 故事页上改故事的路有六条，最常走的那几条
 * （敲字防抖存稿、AI 写眼前这一章、批量展开）一分钟能触发几十次；每次
 * 都问一趟 `/bff/flow` 是拿卡顿换一个对勾。
 *
 * 一致就什么都不做，所以那三条本来就带 `refresh: true` 的路（删章、
 * 直接开写、反推）不会多问一趟：它们的 refresh 先落地，故事再进页面时
 * 两边已经相等。
 */
export function needsFlowReread(storyDone, flowDone) {
  return storyDone !== !!flowDone
}
