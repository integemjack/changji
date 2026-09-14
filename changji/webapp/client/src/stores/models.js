/**
 * 这台机器拿哪几个模型跑。
 *
 * **做成一份共享的状态，不是某个组件的私产。** 项目页那一行只显示名字，
 * 点名字弹出来的窗口改的是同一份东西——两处各拉一遍 `/bff/setup/state`
 * 的话，弹窗里刚换完、页面上那行还是旧名字，而那不报错。
 *
 * 这里**不管模型目录和下载源**：它们是整台机器的设置（用户 2026-09-14：
 * 「模型的路径放到设置里，这个全局统一的」），归设置页。这儿只管"哪一组
 * 用哪一档、下没下、跑不跑得动"。
 *
 * 数据形状（引擎那边 /bff/setup/state）：
 *
 *     groups[]      { key, title, purpose, required, satisfied, options[] }
 *     option        { id, family, familyNote, label, quant, note,
 *                     minVramGb, fits, complete, totalBytes, haveBytes, files[] }
 *     selected{}    组 → 现在配着的那一档 id
 *     recommended{} 组 → 按这张卡推出来的那一档 id
 */

import { defineStore } from 'pinia'
import { computed, ref } from 'vue'

import { api } from '@/api'
import { humanBytes } from '@/composables/useAction'

export const useModels = defineStore('models', () => {
  const state = ref(null)
  /**
   * 编剧那一组真正在跑的模型名。
   *
   * **不能拿目录里那一项的名字顶替。** 目录里编剧那一组是几个"预设"
   * （「智谱 GLM（云端·免费档）」这种），而真正发出去的模型名在
   * `[llm].model` 里，用户在设置页能自己改成 glm-5.3。显示预设名的话，
   * 他配着 glm-5、页面上写着"免费档"——说的和跑的不是一回事。
   */
  const llmModel = ref('')
  /** 编剧那家要不要密钥、填没填。弹窗据此决定露不露那个输入框。 */
  const llmKeyNeeded = ref(false)
  const llmKeySet = ref(false)
  const llmBaseUrl = ref('')
  const llmTemperature = ref(0.7)
  const picks = ref({}) // 组 → 选中的档位 id（还没保存的草稿）
  const progress = ref(null)
  const loading = ref(false)
  const error = ref('')

  let timer = null

  const groups = computed(() => state.value?.groups ?? [])
  const running = computed(() => progress.value?.state === 'running')

  function group(key) {
    return groups.value.find((g) => g.key === key) ?? null
  }

  function pickedOption(g) {
    if (!g) return null
    return g.options.find((o) => o.id === picks.value[g.key]) ?? null
  }

  /**
   * 按流水线顺序排：编剧 → 首帧 → 出片 → 配音。
   *
   * 目录里是 llm / video / image / tts，出片排在首帧前面——而实际是先出
   * 首帧再出片。四个名字并排时顺序就是唯一的线索，排错了等于没有线索。
   */
  const ORDER = ['llm', 'image', 'video', 'tts']

  /**
   * 项目页那一行显示什么。
   *
   * **显示的是"正在用的"，不是草稿。** 弹窗里改到一半还没保存时，
   * 页面上那行该还是旧名字——它说的是"这台机器现在拿什么跑"。
   */
  const inUse = computed(() =>
    [...groups.value]
      .sort((a, b) => ORDER.indexOf(a.key) - ORDER.indexOf(b.key))
      .map((g) => {
        const id = state.value?.selected?.[g.key] || ''
        const opt = g.options.find((o) => o.id === id) ?? null
        return {
          key: g.key,
          title: g.title,
          // 编剧那一组报真正在跑的模型名，别的组报家族名——家族名比档位名
          // 短得多，一行放得下四个：「Qwen-Image」而不是
          // 「Qwen-Image · Q2_K · ≥20 GB 可常驻 · 12.0 GB」
          name:
            (g.key === 'llm' && llmModel.value) ||
            opt?.family ||
            opt?.label ||
            '没挑',
          // 缺东西才标记。齐了什么都不显示——一行四个对勾等于没说。
          missing: needOf(g) > 0,
        }
      }),
  )

  /** 这一组还差多少字节没下。 */
  function needOf(g) {
    const o = pickedOption(g) ?? g?.options?.find((x) => x.id === state.value?.selected?.[g.key])
    if (!o || o.id === 'none') return 0
    return Math.max(0, (o.totalBytes ?? 0) - (o.haveBytes ?? 0))
  }

  /** 把一组的档位按家族拢一拢。一个家族十几档，只该说一遍好话。 */
  function familyChoices(g) {
    const out = []
    const byName = new Map()
    for (const o of g?.options ?? []) {
      if (!o.family) continue
      if (!byName.has(o.family)) {
        const fam = { key: o.family, label: o.family, note: o.familyNote, options: [] }
        byName.set(o.family, fam)
        out.push(fam)
      }
      byName.get(o.family).options.push(o)
    }
    // 「不下载」这类没有家族的，当成只有一档的家族排在最后
    for (const o of (g?.options ?? []).filter((x) => !x.family)) {
      out.push({ key: o.id, label: o.label, note: o.familyNote, options: [o] })
    }
    return out
  }

  function currentFamily(g) {
    const o = pickedOption(g)
    return o ? o.family || o.id : ''
  }

  function currentFamilyChoice(g) {
    return familyChoices(g).find((f) => f.key === currentFamily(g)) ?? null
  }

  /**
   * 换家族时替他挑一档。
   *
   * **不能留在原来那个 id 上**——那个 id 属于上一个家族，换完之后两个
   * 下拉会对不上（家族显示新的、精度还是旧的）。挑的顺序：推荐那档在这个
   * 家族里就用它；否则挑装得下的里面最好的；都装不下就挑最小的（列表从大
   * 到小排，取最后一个）。
   */
  function selectFamily(g, key) {
    const fam = familyChoices(g).find((f) => f.key === key)
    if (!fam?.options?.length) return
    const rec = fam.options.find((o) => o.id === state.value?.recommended?.[g.key])
    const fits = fam.options.filter((o) => o.fits)
    picks.value = {
      ...picks.value,
      [g.key]: (rec ?? fits[0] ?? fam.options[fam.options.length - 1]).id,
    }
  }

  /**
   * 这张卡跑不跑得动，**给结论不给数**。
   *
   * 原来是一个 pill 写「≥ 20 GB 放内存，慢」，另一处写「显卡 6.0 GB」，
   * 要用户自己拿两个数去比——而引擎早就算好了（`fits`），只是没说人话。
   */
  function verdict(o) {
    if (!o || o.id === 'none') return ''
    if (!o.minVramGb) return '不占显存'
    return o.fits ? '常驻得下' : '放不下，会慢'
  }

  /** 精度下拉里一行怎么写。选项里没法排版，只能全塞进这一行字。 */
  function optionLine(g, o) {
    const marks = []
    if (state.value?.recommended?.[g.key] === o.id) marks.push('推荐')
    if (o.complete) marks.push('已下好')
    const head = `${o.quant || o.label} · ${humanBytes(o.totalBytes)}`
    return marks.length ? `${head} · ${marks.join('、')}` : head
  }

  async function load() {
    loading.value = true
    error.value = ''
    try {
      const data = await api.setupState()
      state.value = data
      progress.value = data.download
      // **已经配着的优先，没有才用推荐的。顺序不能反**——反了的话，
      // 用户上次特意挑的小一档会被换回推荐档，而他多半不会注意到。
      const kept = {}
      for (const [k, v] of Object.entries(data.selected || {})) if (v) kept[k] = v
      picks.value = { ...data.recommended, ...kept }
      // 编剧那一组显示的是真正在跑的模型名，不是目录里那个预设名
      try {
        const c = await api.connections()
        llmModel.value = c?.llm_model || ''
        llmKeySet.value = Boolean(c?.llm_api_key_set)
        llmBaseUrl.value = c?.llm_base_url || ''
        llmTemperature.value = Number(c?.llm_temperature ?? 0.7)
        // 本机/局域网的服务不校验密钥（Ollama 那些），云端才要
        llmKeyNeeded.value = !/\/\/(127\.0\.0\.1|localhost|0\.0\.0\.0|192\.168\.|10\.)/.test(
          c?.llm_base_url || '',
        )
      } catch {
        llmModel.value = '' // 问不到就退回预设名，不该让整页红
      }
      if (data.download?.state === 'running') poll()
    } catch (err) {
      error.value = err.message
    } finally {
      loading.value = false
    }
  }

  function poll() {
    if (timer) return
    timer = setInterval(async () => {
      try {
        progress.value = await api.setupProgress()
        if (progress.value?.state !== 'running') {
          stopPoll()
          await load()
        }
      } catch {
        stopPoll()
      }
    }, 1000)
  }

  function stopPoll() {
    clearInterval(timer)
    timer = null
  }

  /**
   * 只存这一组的选择，不下。
   *
   * ⚠️ **只把这一组报上去。** 引擎收到 selections 会顺手把这几组的配置
   * 写实（见 post_setup_download）；把整份 picks 都发过去的话，别的组里
   * 改了还没保存的选择会被一起写进配置——而用户按的只是这一个弹窗的保存。
   */
  async function saveGroup(key) {
    const res = await api.startSetupDownload({
      selections: { [key]: picks.value[key] },
      download: false,
    })
    progress.value = res.progress
    await load()
    return needOf(group(key))
  }

  /** 下这一组缺的。理由同上：只报这一组。 */
  async function downloadGroup(key) {
    const res = await api.startSetupDownload({ selections: { [key]: picks.value[key] } })
    progress.value = res.progress
    if (res.started) {
      poll()
      return true
    }
    // 一个文件都不用碰（选的是「不下载」，或者盘上都有）。引擎连一轮都没起。
    await load()
    return false
  }

  async function stop() {
    progress.value = await api.cancelSetupDownload()
    stopPoll()
  }

  return {
    state, picks, progress, loading, error, llmModel, llmKeyNeeded, llmKeySet, llmBaseUrl, llmTemperature,
    groups, running, inUse,
    group, pickedOption, needOf, familyChoices, currentFamily, currentFamilyChoice,
    selectFamily, verdict, optionLine,
    load, saveGroup, downloadGroup, stop,
  }
})
