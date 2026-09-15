<script setup>
/**
 * 改一个模型。弹窗，一次只管一组。
 *
 * 用户 2026-09-14：「模型只要显示用的模型名字，点击名字弹出显示内容和
 * 更换模型」。
 *
 * **一次只管一组，不是四组一张表。** 用户点的是某一个名字，他这一刻要
 * 决定的就是那一个；把另外三组也摆出来，等于又回到那张要上下滚的长单子。
 *
 * **这儿不管模型目录和下载源**——那是整台机器的设置，归设置页
 * （「模型的路径放到设置里，这个全局统一的」）。这儿只有显卡和盘剩余两个
 * 读数，因为"这一档跑不跑得动、装不装得下"要靠它们才说得清。
 *
 * ---- 编剧那一组不一样 ----
 *
 * 另外三组挑的是"下哪一份权重"，编剧挑的是"用云上哪个模型"——它一个字节
 * 都不下。目录里那几项只是**预设**（「智谱 GLM（云端·免费档）」），而真正
 * 发出去的模型名在 `[llm].model` 里。
 *
 * 用户 2026-09-14：「在线模型的名字是不是也应该在这设置」。之前是：项目页
 * 显示 `glm-4.5-air`，点进去却只能换预设，改名字要跑去设置页——**看得见
 * 改不了，比不显示更别扭**。所以这一组多两样：模型名（服务上真有的 +
 * 我们那本小抄）和密钥。
 *
 * 密钥那一格**一直摆着**，不按"填没填过"隐藏——第一版是填过就藏起来，
 * 而在这儿能换服务之后那就是个真 bug：从智谱换到 DeepSeek，旧密钥还在、
 * 框不出现，新家的密钥没地方填。理由的全文在模板里那一格上。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import ProgressBar from '@/components/ProgressBar.vue'
import { api } from '@/api'
import { humanBytes, humanRate, humanTime } from '@/composables/useAction'
import { useModels } from '@/stores/models'
import { useUi } from '@/stores/ui'

const props = defineProps({
  open: { type: Boolean, default: false },
  groupKey: { type: String, default: '' },
})
const emit = defineEmits(['close'])
/**
 * Esc 关掉。
 *
 * 抽屉那几处早就有（「Esc 是唯一不用先瞄准的出口」），而**弹窗比抽屉更该
 * 有**——它盖住整屏，除了右上角那个 ✕ 和点外面，没别的出路。三个弹窗原来
 * 一个都不认 Esc。
 *
 * 判 `props.open`：这个组件是**一直挂着**的（`v-if` 在模板里面），不判的话
 * 它在窗口关着的时候也吃 Esc。
 */
function onEsc(e) {
  if (e.key === 'Escape' && props.open) emit('close')
}
onMounted(() => window.addEventListener('keydown', onEsc))
onUnmounted(() => window.removeEventListener('keydown', onEsc))
/**
 * 打开之后把焦点放进来。
 *
 * **不然键盘上这个弹窗基本没法用。** 用 Tab 走到那颗按钮、回车打开，焦点
 * 还停在**遮罩后面**那颗按钮上：按 Tab 是在看不见的页面里一格格走，走到
 * 弹窗里之前，屏幕上一个焦点框都看不见。
 *
 * 焦点落在面板本身（tabindex="-1"），不猜第一个该聚焦的控件——猜错了会把
 * 人直接丢进某个输入框，而读屏也该先听见这是个什么窗。再按 Tab 就顺着进
 * 里面的控件了。
 */
const panel = ref(null)
// **盯着面板出现，不是盯着 open 变真。** 面板挂在内容上（拍摄那个是
// `v-if="video"`，模型那个是 `v-if="g"`），而内容是打开之后异步读回来的
// ——按 open 那一刻去聚焦会扑空，而扑空的表现正是这条要修的：焦点留在
// 遮罩后面。模板 ref 本身是响应式的，元素挂上来就聚焦，卸了就是 null。
watch(panel, (el) => el?.focus())

/**
 * 关掉之后把焦点还回去。
 *
 * 不还的话焦点落在 `<body>` 上：下一次按 Tab 是从整页开头重走，而人刚才
 * 站在页面中间那颗按钮上。开的时候记一下是谁把它叫起来的，关的时候还给它
 * ——那颗按钮已经不在了（比如刚被这次操作删掉）也没关系，focus 一个不在
 * 文档里的元素什么都不会发生。
 */
let opener = null
watch(
  () => props.open,
  (now) => {
    if (now) {
      opener = document.activeElement
      return
    }
    const back = opener
    opener = null
    back?.focus?.()
  },
)

const models = useModels()
const ui = useUi()
const busy = ref(false)

const isLlm = computed(() => props.groupKey === 'llm')

/** 编剧那一组：服务上有哪些模型、我们的小抄、现在用的是哪个。 */
const llm = ref(null)
const llmPick = ref('')
/**
 * 有哪几家服务可选。
 *
 * **从 `/api/llm/providers` 来，不是从模型目录来。** 目录里编剧那一组只有
 * 两项：智谱、和一句含糊的「不下载 · 用别的外接服务」——用户 2026-09-14
 * 的判词是「编剧模型怎么就只有智谱了，还有多个外部服务什么玩意」。
 * 而引擎本来就带着一份十五家的接入地址清单（Ollama、LM Studio、vLLM、
 * DeepSeek、硅基流动、百炼、Moonshot、火山方舟、混元、MiniMax…），
 * 那份才是这个下拉该显示的东西。
 */
const providers = ref([])
const apiKey = ref('')
const baseUrl = ref('')
const temp = ref(0.7)

/**
 * 下拉里摆哪些。**只有模型名，不带任何说明。**
 *
 * 上一版每一项后面挂着一句评语（「这一档最会写：写作榜 81.8、套话 7.09，
 * 八章几乎不降」）——用户 2026-09-14 的判词是"显示那么多废话干什么"。
 * 一个下拉里摆十几行长句，要挑的那个名字反而找不着。
 *
 * 列表从服务那边拉（`/api/llm/models` 的 models），再把我们那本小抄里的
 * **id** 并进来——**只并 id，不并说明**。并的理由是智谱的 `/models`
 * 不列免费档（glm-4.7-flash 就在里面），而那恰恰是默认那一个；不并的话
 * 默认模型在下拉里找不到。
 */
/** 现在这个地址是哪一家。认不出就是「自定义」。 */
const providerId = computed(() => {
  const here = (baseUrl.value || '').replace(/\/+$/, '')
  return providers.value.find((p) => p.base_url.replace(/\/+$/, '') === here)?.id || ''
})

/**
 * 密钥框里那句提示。
 *
 * ⚠️ **`llmKeySet` 说的是"存过一把"，不是"这一家的存过"。** 配置里只有
 * 一个 api_key，换家不换它。于是从智谱换到 DeepSeek 时，框里照旧写着
 * 「已填过，留空就不改」——照着留空存下去，发给 DeepSeek 的是智谱那把，
 * 要到第一次写稿才 401，而那时人早忘了这一步。
 *
 * 这正是上面那个「密钥一直摆着别按填没填过隐藏」要修的同一件事，只是
 * 当初只把框露出来了，框里那句话没跟着改。
 */
const keyHint = computed(() => {
  const trim = (u) => (u || '').trim().replace(/\/+$/, '')
  const here = trim(baseUrl.value)
  const saved = trim(models.llmBaseUrl)
  if (here && saved && here !== saved) return '换了一家，这家的密钥要重新填'
  if (models.llmKeySet) return '已填过，留空就不改'
  return models.llmKeyNeeded ? '这家要密钥' : '本机服务一般不用填'
})

/** 换一家：把地址换过去，模型列表跟着重拉。 */
async function pickProvider(id) {
  const p = providers.value.find((x) => x.id === id)
  if (!p) return
  baseUrl.value = p.base_url
  await reloadModels()
}

const llmChoices = computed(() => {
  const seen = new Set()
  const out = []
  for (const m of llm.value?.models ?? []) {
    if (seen.has(m)) continue
    seen.add(m)
    out.push(m)
  }
  for (const k of llm.value?.known ?? []) {
    if (seen.has(k.id)) continue
    seen.add(k.id)
    out.push(k.id)
  }
  // 现在用的那个要在列表里，否则下拉会显示空白
  if (llmPick.value && !seen.has(llmPick.value)) out.unshift(llmPick.value)
  return out.sort()
})

const g = computed(() => models.group(props.groupKey))
const opt = computed(() => models.pickedOption(g.value))
const need = computed(() => models.needOf(g.value))
const fam = computed(() => models.currentFamilyChoice(g.value))

/** 这一档有几档可挑。只有一档就不摆那个下拉——摆一个没得选的框是噪音。 */
const hasQuants = computed(() => (fam.value?.options?.length ?? 0) > 1)

const diskFree = computed(() => Number(models.state?.diskFreeBytes ?? 0))
const gpu = computed(() => models.state?.gpu ?? null)

/** 盘装不装得下这一次要下的。装不下是**红字**，因为下到一半才发现最难受。 */
const diskShort = computed(
  () => need.value > 0 && diskFree.value > 0 && need.value > diskFree.value,
)

/** 这一组正在下的那份进度。 */
const stat = computed(() => {
  const p = models.progress
  if (!p?.items?.length) return null
  // 快照里每一项都带 group，按它挑出这一组的几个文件
  const mine = p.items.filter((i) => i.group === props.groupKey)
  if (!mine.length) return null
  let total = 0
  let done = 0
  let speed = 0
  let active = false
  let failed = false
  const notes = []
  for (const i of mine) {
    total += Number(i.total) || 0
    done += Number(i.downloaded) || 0
    if (i.state === 'running') speed += Number(i.speedBps) || 0
    if (i.state === 'running' || i.state === 'pending') active = true
    if (i.state === 'failed') failed = true
    // 这一项自己的说法。引擎写得很具体（"起不来 aria2c"、"下了 N 字节，
    // 应该是 M"），而这儿原来只把它们并成一个 failed 布尔。
    if (i.error) notes.push(`${i.name}：${i.error}`)
  }
  // **整趟活儿挂掉那一种，一项都没轮到。** 找不到 aria2c / curl，或者
  // 建不了模型目录——downloader.cpp 的 `fatal` 那两条——items 全停在
  // pending 上，谁都不是 failed。于是下面那个三目走到最后一支，屏幕上
  // 是一条绿的「这一组齐了」钉在 0%，而一个字节都没下。
  const dead = p.state === 'failed'
  return {
    percent: total ? Math.min(100, (done / total) * 100) : 0,
    running: models.running && active,
    failed: failed || dead,
    // **没跑完也不是"齐了"。** 人自己按了停（RunState::Canceled）之后
    // items 是 canceled，谁都不是 failed，于是下面那个三目一样落到最后
    // 一支——一条绿的「这一组齐了」，旁边紧挨着写「5.1 GB / 8.2 GB」，
    // 底下那颗按钮还写着「下载 8.2 GB」。三句话互相打架。
    short: total > 0 && done < total,
    // 出了事说人话。每一项自己的说明优先——引擎在整体那句里写的就是
    // 「看下面每一项的说明」，而这一版之前**一项的说明都没地方看**。
    // 一项都没说法（fatal 那两条正是这样）才退回整体那一句，那一句里
    // 装着"装哪个下载器"这类唯一能指望的出路。
    why: notes.length ? notes.join('\n') : dead ? String(p.error || '') : '',
    speed,
    downloaded: done,
    total,
  }
})

// 每次打开都重读一遍。别的地方（另一个弹窗、设置页）可能刚改过。
watch(
  () => props.open,
  async (now) => {
    if (!now) return
    models.load()
    if (!isLlm.value) return
    apiKey.value = ''
    baseUrl.value = models.llmBaseUrl
    temp.value = models.llmTemperature
    try {
      providers.value = (await api.llmProviders())?.providers ?? []
    } catch {
      providers.value = [] // 清单拉不到就只剩地址框，照样能填
    }
    await reloadModels()
  },
)

/**
 * 拉这家服务有哪些模型。
 *
 * **换一家之后一定要重拉。** 上一家的模型名在这一家多半不存在，
 * 留着的话下拉里摆的是一串这家根本没有的名字，而点保存要到第一次生成
 * 才报错。
 */
async function reloadModels() {
  try {
    llm.value = await api.llmModels()
    llmPick.value = llm.value.current || ''
    // **问不到也是 200。** llm_info.cpp 那三条失败（连不上 / 对面回
    // 4xx、5xx / 回的不是 JSON）一律「返回空列表加一句原因」，刻意不抛
    // ——「列不出来不该让整个设置页打不开」。于是这一趟在 fetch 那一层
    // 是成功的，下面那个 catch 一次都轮不到。
    //
    // 而列表空着这件事**看不出来**：llmChoices 拉不到就退回我们自己那本
    // 小抄（known），下拉里照样是一串眼熟的模型名。填了个打错的地址、
    // 或者 Ollama 压根没起来，这个窗从头到尾一声不吭，要到第一次写剧本
    // 才报错。引擎把话说得很具体（「连不上 http://…：Connection
    // refused」），一直没人念出来。
    if (llm.value?.error) ui.warn(llm.value.error)
  } catch (e) {
    // 连不上那家（地址刚换、密钥还没填）——模型列表空着，地址和密钥照样能存
    llm.value = { models: [], known: [], current: '' }
    ui.warn(`问不到这家有哪些模型：${e.message}`)
  }
}

async function save() {
  busy.value = true
  try {
    // **编剧那一组不走模型目录。**
    //
    // 目录里那两项（智谱 / 别的外接服务）是预设，存它会把预设里写死的
    // `llm.base_url` 和 `llm.model` 一起写进配置——正好盖掉用户刚在这儿
    // 挑的那家和那个模型名。而地址、模型、密钥现在全在这个弹窗里，
    // 预设没有任何还要它的理由。
    if (isLlm.value) {
      const patch = {}
      const url = baseUrl.value.trim()
      if (url && url !== models.llmBaseUrl) patch.llm_base_url = url
      if (llmPick.value && llmPick.value !== models.llmModel) {
        patch.llm_model = llmPick.value
      }
      if (apiKey.value.trim()) patch.llm_api_key = apiKey.value.trim()
      if (Number(temp.value) !== models.llmTemperature) {
        patch.llm_temperature = Number(temp.value)
      }
      // **要包一层 `patch`。** 引擎那边先找 body.patch，直接发裸字段是 422
      // （`patch` Field required）——而 422 的消息里不会提"你少包了一层"，
      // 只会说缺 patch，看着像少填了什么东西。
      // `persist: true` 把密钥落到配置目录里那个单独的 api_key 文件。
      if (Object.keys(patch).length) {
        await api.saveConnections({ patch, persist: true })
      }
      await models.load()
      ui.ok('换好了')
      emit('close')
      return
    }

    const left = await models.saveGroup(props.groupKey)
    ui.ok(left > 0 ? `换好了，还差 ${humanBytes(left)} 没下` : '换好了')
    emit('close')
  } catch (e) {
    ui.error(e.message)
  } finally {
    busy.value = false
  }
}

/**
 * 停下这一轮下载。
 *
 * **要接住错。** 这儿原来是模板里直接 `@click="models.stop()"`，而 store
 * 里那个 stop 是裸的 `await api.cancelSetupDownload()`——取消发不出去（引擎
 * 正忙、连接断了）就是一个没人接的 Promise 拒绝：按钮点下去没反应，进度条
 * 还在走，用户只会再点几下。旁边 save / download 两个都是包着的。
 */
async function stopDownload() {
  try {
    await models.stop()
  } catch (e) {
    ui.error('停不下来：' + e.message)
  }
}

async function download() {
  busy.value = true
  try {
    // **不关窗。** 下载要几十分钟，关掉之后进度就只剩项目页那一行了；
    // 留着的话用户能看着它走，想走开再关。
    const started = await models.downloadGroup(props.groupKey)
    if (!started) ui.ok('盘上都有了，配置已经指过去')
  } catch (e) {
    ui.error(e.message)
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div v-if="open" class="mask" @click.self="emit('close')">
    <section v-if="g" ref="panel" class="dlg" tabindex="-1">
      <header class="dlg__head">
        <div class="dlg__title">
          <h2 class="dlg__t">{{ g.title }}</h2>
          <!-- 这一组是干什么的。**放标题下面不放同一行**：同一行时它会把
               标题挤成三行，而标题才是"我点开的是哪一个"的答案。 -->
          <p class="tiny dim purpose">{{ g.purpose }}</p>
        </div>
        <span class="spacer" />
        <span class="tiny dim nowrap">
          <template v-if="gpu">{{ gpu.name }} · {{ gpu.vramGb.toFixed(1) }} GB</template>
          <template v-else>没探测到显卡</template>
          <template v-if="diskFree"> ｜ 盘剩 {{ humanBytes(diskFree) }}</template>
        </span>
        <button
          class="btn btn--ghost btn--sm"
          type="button"
          aria-label="关闭"
          @click="emit('close')"
        >
          <AppIcon name="close" :size="14" />
        </button>
      </header>

      <div class="dlg__body stack stack--sm">
        <!-- 编剧：挑哪一家。清单从引擎来，十五家。 -->
        <label v-if="isLlm" class="field">
          <span class="field__label">服务</span>
          <select :value="providerId" class="select" @change="pickProvider($event.target.value)">
            <option value="">自定义（自己填地址）</option>
            <option v-for="p in providers" :key="p.id" :value="p.id" :title="p.note">
              {{ p.name }}
            </option>
          </select>
        </label>

        <!-- 下权重那三组：挑哪个模型。 -->
        <label v-else class="field">
          <span class="field__label">模型</span>
          <select
            class="select"
            :disabled="models.running"
            :value="models.currentFamily(g)"
            @change="models.selectFamily(g, $event.target.value)"
          >
            <option v-for="f in models.familyChoices(g)" :key="f.key" :value="f.key">
              {{ f.label }}
            </option>
          </select>
        </label>

        <!-- 编剧那一组：真正发出去的模型名。**这才是项目页上显示的那个**，
             上面那个下拉挑的只是"哪一家"。 -->
        <label v-if="isLlm" class="field">
          <span class="field__label">模型</span>
          <select v-model="llmPick" class="select">
            <option v-for="m in llmChoices" :key="m" :value="m">{{ m }}</option>
          </select>
        </label>

        <!-- 地址。**它得在这儿**，不然上面那个下拉选了「用别的外接服务」
             就没地方填，用户点完发现是条死路。 -->
        <label v-if="isLlm" class="field">
          <span class="field__label">接口地址</span>
          <input v-model="baseUrl" class="input mono" placeholder="https://…/v1" />
        </label>

        <!-- 密钥。**一直摆着，不按"填没填过"隐藏。**
             上一版是填过就藏起来，而在这儿能换服务之后那是个真 bug：
             从智谱换到 DeepSeek 时旧密钥还在，框就不出现——新家的密钥
             没地方填。留空表示不改。 -->
        <label v-if="isLlm" class="field">
          <span class="field__label">API Key</span>
          <input
            v-model="apiKey"
            class="input mono"
            type="password"
            :placeholder="keyHint"
          />
        </label>

        <!-- 温度。**从设置页搬过来的**（那一节整个删了）：地址、模型、密钥
             都在这儿之后，再让人为一个数字跑一趟设置页没道理。 -->
        <label v-if="isLlm" class="field">
          <span class="field__label">温度</span>
          <input
            v-model.number="temp"
            class="input numeric"
            type="number"
            step="0.1"
            min="0"
            max="2"
            title="越高越发散。写大纲那几步会在这个数上再往上加一档"
          />
        </label>

        <label v-if="hasQuants" class="field">
          <span class="field__label">精度</span>
          <select v-model="models.picks[g.key]" class="select" :disabled="models.running">
            <option v-for="o in fam.options" :key="o.id" :value="o.id">
              {{ models.optionLine(g, o) }}
            </option>
          </select>
        </label>

        <!-- 这一档的读数。**给结论，不给你两个数自己比。** -->
        <!-- 云端那一档一个字节都不下，这一行整个不出现——
             摆一句「· 不占显存」在那儿，读起来像少了半句话。 -->
        <p v-if="opt?.totalBytes" class="row tiny">
          <span class="numeric">{{ humanBytes(opt.totalBytes) }}</span>
          <span v-if="models.verdict(opt)" class="dim">· {{ models.verdict(opt) }}</span>
          <span v-if="opt.complete" class="dim">· 盘上已有</span>
        </p>

        <!-- 那一档的一句话。**编剧那一组不显示**：它挑的是云上哪个模型，
             而那一段是在讲"云端这条路好在哪"——和此刻要做的决定没关系。
             下权重那三组留着，那一句是挑精度的唯一依据。 -->
        <p v-if="!isLlm && opt?.note" class="tiny dim note">{{ opt.note }}</p>

        <p v-if="diskShort" class="tiny bad">
          要下 {{ humanBytes(need) }}，而盘只剩 {{ humanBytes(diskFree) }}，装不下。
          去设置页换个模型目录，或者先删掉一些。
        </p>

        <!-- 轮询断了之后那句话。`models.error` 之前只有项目页那一行读，
             而下载正跑着的时候人盯的是这个窗。 -->
        <p v-if="models.pollError" class="tiny danger-text">{{ models.pollError }}</p>

        <div v-if="stat" class="stack stack--sm">
          <!-- **总大小还不知道的时候，别画一条停在 0% 的进度条。**
               `stat.percent` 是 `total ? done/total : 0`——下载刚起步（文件
               还是 pending，拿不到 Content-Length），或者对面干脆不发这个头，
               total 就是 0，于是进度条钉在最左边、旁边写着「0 B / 0 B」，
               而下面同时还在报速度。几个 G 的模型这一段可能持续很久，看着
               就是卡死了。
               ProgressBar 早就为这件事留了 `indeterminate`（连来回跑的那段
               动画都写好了），只是一直没人接上。总大小不知道时那行字也只报
               已经下了多少——这是此刻唯一说得准的数。 -->
          <ProgressBar
            :percent="stat.percent"
            :indeterminate="stat.running && !stat.total"
            :tone="stat.failed ? 'danger' : stat.running || stat.short ? 'accent' : 'ok'"
            :label="
              stat.failed
                ? '有文件没下下来'
                : stat.running
                  ? '正在下'
                  : stat.short
                    ? '没下完，再下一次会从断点接着'
                    : '这一组齐了'
            "
            :detail="
              stat.total
                ? `${humanBytes(stat.downloaded)} / ${humanBytes(stat.total)}`
                : humanBytes(stat.downloaded)
            "
          />
          <!-- 出事了的那句原话。上面那条只有颜色和「有文件没下下来」五个字，
               而**为什么**一直被丢掉：这台机器上没有 aria2c/curl 时引擎
               给的是三行「装一个就行」的命令，是唯一的出路。 -->
          <p v-if="stat.why" class="tiny danger-text lines">{{ stat.why }}</p>
          <div v-if="stat.running" class="row row--between tiny dim">
            <span class="numeric">
              <template v-if="stat.speed > 0">{{ humanRate(stat.speed) }}</template>
              <template v-if="models.progress?.etaSeconds > 0">
                · 还要 {{ humanTime(models.progress.etaSeconds) }}
              </template>
            </span>
            <button class="btn btn--ghost btn--sm" type="button" @click="stopDownload">
              停下
            </button>
          </div>
        </div>
      </div>

      <footer class="dlg__foot">
        <span class="spacer" />
        <button
          v-if="need > 0 && !stat?.running"
          class="btn btn--sm"
          type="button"
          :disabled="busy"
          @click="download"
        >
          <!-- 下载用 download，不是 upload——那个箭头是从托盘里往**上**
               飞出去的，印在「下载 8.2 GB」上是反的。另外三处用 upload 的
               地方（去上传、投递）是真的往外送。 -->
          <AppIcon name="download" :size="14" />
          下载 {{ humanBytes(need) }}
        </button>
        <button
          class="btn btn--primary btn--sm"
          type="button"
          :disabled="busy || models.running"
          @click="save"
        >
          保存
        </button>
      </footer>
    </section>
  </div>
</template>

<style scoped>
/* 引擎那几条说明是带换行的（"装一个就行"底下三行命令），别挤成一行 */
.lines {
  white-space: pre-line;
}

.mask {
  position: fixed;
  inset: 0;
  z-index: 80;
  display: grid;
  place-items: center;
  padding: var(--s3);
  background: rgb(0 0 0 / 45%);
}

.dlg {
  width: min(560px, 100%);
  max-height: 86vh;
  display: flex;
  flex-direction: column;
  border: 1px solid var(--line);
  border-radius: 12px;
  background: var(--surface);
  box-shadow: 0 20px 60px rgb(0 0 0 / 35%);
}

.dlg__head,
.dlg__foot {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 14px;
}

.dlg__head {
  align-items: flex-start;
  border-bottom: 1px solid var(--line);
}

.dlg__title {
  min-width: 0;
}

.purpose {
  margin: 2px 0 0;
  line-height: 1.5;
}

.dlg__foot {
  border-top: 1px solid var(--line);
}

.dlg__t {
  margin: 0;
  font-size: var(--fs-md);
  font-weight: 600;
}

.dlg__body {
  padding: 14px;
  overflow-y: auto;
}

.note {
  margin: 0;
  line-height: 1.6;
}

.bad {
  margin: 0;
  color: var(--danger, #e5484d);
}
</style>
