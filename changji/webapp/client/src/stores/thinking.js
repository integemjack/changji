/**
 * 模型现在在想什么。
 *
 * 用户 2026-09-14：「不管写文章还是什么都要显示出在思考的 UI，鼠标放上去
 * 还能看到内容」。
 *
 * **为什么非有不可。** 现在的模型都要"先想再写"，而且关不掉（智谱
 * glm-5.3 发关闭值直接回 400）。思考少则十几秒、多则十几分钟，这段时间里
 * 正文一个字都不出来——用户面对的是一个一动不动的进度条，分不清是在想
 * 还是卡死了。实测 glm-5.3 写一份大纲，思考那一段就占了十分钟以上。
 *
 * **做成全局的一份，不是每个视图各存各的。** 会思考的步骤有十几个（写
 * 大纲、写正文、写剧本、拆分镜、定妆、提人物…），散在五六个视图里。
 * 让每个视图自己接一遍的话，漏掉的那几个的表现是"这一步没有思考显示"，
 * 而且不报错。现在统一由 runAsyncJob 往这儿写，顶栏那个徽标统一读。
 *
 * 存量是有上限的：一段思考能有几千上万字，十几步攒下来会把内存和渲染
 * 都吃掉。每条只留最后 kMaxChars 个字——浮层里要看的本来就是"它**现在**
 * 在想什么"，翻到十分钟前那段没有意义。
 */

import { defineStore } from 'pinia'
import { computed, ref } from 'vue'

import { api } from '@/api'

/** 单条最多留多少字。超了从前面丢。 */
const MAX_CHARS = 4000

/**
 * 排序用的自增号。
 *
 * **不能拿 `at`（时间戳）排。** 批量那条路上好几步是连着开的，落在同一
 * 毫秒里 `Date.now()` 一样，排出来的先后就成了随机的——浮层里显示"当前
 * 这一条"会在两条之间跳。`at` 留着是给"想了多久"用的。
 */
let seq = 0

export const useThinking = defineStore('thinking', () => {
  /** streamId → { label, text, at } */
  const live = ref({})

  /** 开一条。`label` 是给人看的（"写大纲"这种）。 */
  function start(streamId, label) {
    if (!streamId) return
    live.value = {
      ...live.value,
      [streamId]: { label: label || '', text: '', at: Date.now(), seq: ++seq },
    }
  }

  /** 来了一段思考。**只收增量**，拼接是这儿的事。 */
  function push(streamId, piece) {
    if (!streamId || !piece) return
    const cur = live.value[streamId]
    // 没 start 过也收：socket 上的消息可能比 start 先到，丢掉的话
    // 这一步就永远不显示了。
    const text = (cur?.text ?? '') + piece
    live.value = {
      ...live.value,
      [streamId]: {
        label: cur?.label ?? '',
        at: cur?.at ?? Date.now(),
        seq: cur?.seq ?? ++seq,
        text: text.length > MAX_CHARS ? text.slice(text.length - MAX_CHARS) : text,
      },
    }
  }

  /** 这一步完了（成了或者砸了都算）。 */
  function finish(streamId) {
    if (!streamId || !(streamId in live.value)) return
    const next = { ...live.value }
    delete next[streamId]
    live.value = next
  }

  /**
   * 引擎那本任务账里正在想的那几条：`id → { label, text, end }`。
   *
   * ⚠️ **上面那份 `live` 刷新一下就没了。**
   *
   * 它按 `streamId` 存，而那串字是**点下去的时候浏览器随机生成的**——刷新
   * 之后既不知道有活在跑，也没法订上它。而这一族活少则十几秒、多则十几
   * 分钟，刷新期间正好在想是常态，不是边角情况。用户 2026-09-17：「顶部的
   * 思考刷新后就再也不显示」。
   *
   * `useRefStream` 早就为同一件事修过一遍（出参考图那条固定频道），只是
   * 思考这一族漏了。现在引擎那本任务账（`/api/tasks`）带着每一件在跑的活
   * 和它想了多少字，按**任务 id**索引——那个数刷新之后照样在。
   *
   * **socket 有东西的时候一行都不看**，和 useRefStream 那条退路一个规矩：
   * 本标签页自己点的那次，socket 快一拍半，而且两边同时显示会成双份。
   */
  const fromBoard = ref({})

  /** 跟引擎那本账对一次。socket 上有东西就不看。 */
  async function syncFromServer() {
    if (Object.keys(live.value).length > 0) {
      if (Object.keys(fromBoard.value).length > 0) fromBoard.value = {}
      return
    }
    let rows = []
    try {
      rows = (await api.tasks()).running ?? []
    } catch {
      return   // 问不到就保持上一拍，别把浮层闪没了
    }
    const next = {}
    for (const r of rows) {
      if (!r.thinking) continue
      const have = fromBoard.value[r.id]
      let text = have?.text ?? ''
      let end = have?.end ?? 0
      try {
        const d = await api.taskThinking(r.id, end)
        const start = Number(d?.start ?? 0)
        // start 大于手上那份的结尾 = 中间被截掉一段，丢掉重接。
        text = have && start <= end ? text + (d?.thinking ?? '') : (d?.thinking ?? '')
        end = Number(d?.end ?? 0)
      } catch {
        // 这一条取不到就先留着旧的
      }
      next[r.id] = {
        label: r.title || '',
        at: Date.now() - Math.round((r.seconds || 0) * 1000),
        seq: Number(r.id),
        text: text.length > MAX_CHARS ? text.slice(text.length - MAX_CHARS) : text,
        end,
      }
    }
    fromBoard.value = next
  }

  /** 正在想的那几条，新的在前。 */
  const items = computed(() => {
    const src =
      Object.keys(live.value).length > 0 ? live.value : fromBoard.value
    return Object.entries(src)
      .map(([id, v]) => ({ id, ...v }))
      .sort((a, b) => b.seq - a.seq)
  })

  const busy = computed(() => items.value.length > 0)

  /** 最近那一条的正文，浮层默认显示它。 */
  const latest = computed(() => items.value[0]?.text ?? '')

  return { live, items, busy, latest, start, push, finish, syncFromServer }
})
