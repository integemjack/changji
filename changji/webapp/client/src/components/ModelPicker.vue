<script setup>
/**
 * 挑模型、下模型。**初始化页和设置页共用这一个组件。**
 *
 * 为什么共用而不是各写一份：两处要做的事是同一件——看这台机器该用哪一档、
 * 换一档、把缺的下下来。分成两份的话，量化档的说明、显存门槛、
 * 「换家族要清空上一家的键」这些迟早只改一边，而分家的表现是
 * 设置页写着一套、初始化页写着另一套，用户不知道该信哪个。
 *
 * 两处唯一的差别在外面那圈：初始化页是一整页带「先跳过 / 进入首页」，
 * 设置页是一节。所以那两个按钮走插槽，组件自己不管跳转。
 *
 * ---
 *
 * 三条界面上的取舍：
 *
 * **推荐是选好的，不是标出来的。** 进来时每一组已经选中一项——已经配着的
 * 优先，没配过才用按显卡推的那一档。只标一个「推荐」角标让用户自己去点的话，
 * 多数人会挑最大的那个，然后在出片时 OOM——那个错要跑几分钟才出现，
 * 还看不出和这一页的选择有关。
 *
 * **装不下的不禁选，只说代价。** 卡小但内存大的机器把权重放内存照样跑得动，
 * 只是慢。禁掉等于替用户做主，而我们并不知道他的内存有多大。
 *
 * **进度是轮询问出来的，不是推过来的。** 43 GB 要下几个小时，这期间用户会
 * 刷新页面、关掉浏览器第二天回来、换台设备看。轮询这三种都对，
 * 事件流每一种都要另写一段补偿。
 */
import { computed, onMounted, onUnmounted, ref } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import ProgressBar from '@/components/ProgressBar.vue'
import { api } from '@/api'
import { humanBytes, humanRate, humanTime } from '@/composables/useAction'
import { clearSetupCheck, markSetupHandled } from '@/composables/useSetupGate'
import { useUi } from '@/stores/ui'

defineProps({
  // 设置页那张卡里用。收紧留白，去掉几句只有第一次运行才需要的话。
  // 模板里直接读 dense，不用接住返回值。
  dense: { type: Boolean, default: false },
})

// 配置真的写进去了。设置页要靠它重读一遍——步数、画幅这些跟着模型变。
const emit = defineEmits(['applied'])

const ui = useUi()

const state = ref(null)
const loading = ref(true)
const loadError = ref('')
const picks = ref({}) // { 组: 选项 id }
const dir = ref('')
// 从哪儿下。空 = 跟引擎探出来的那个走。
const source = ref('')
const starting = ref(false)
const progress = ref(null)
const expanded = ref({}) // 哪几组展开了文件明细

let timer = null

const groups = computed(() => state.value?.groups ?? [])
const running = computed(() => progress.value?.state === 'running')
const finished = computed(
  () => progress.value?.state === 'done' && (progress.value?.items?.length ?? 0) > 0,
)

/** 这一组现在选中的那一项，和它包含的文件。模板里要用好几处。 */
function pickedOption(g) {
  return g.options.find((o) => o.id === picks.value[g.key]) ?? null
}
function pickedFiles(g) {
  return pickedOption(g)?.files ?? []
}

/**
 * 把一组的选项按家族拢一拢。
 *
 * **一个家族的十几档量化只该说一遍好话。** Qwen-Image 有 15 档、
 * Wan 有 14 档，每档都把家族那段话重复一遍的话，整页就没法看了——
 * 而那段话（好在哪、代价是什么）恰恰是用户挑家族时唯一要读的东西。
 * 所以家族说明放在小标题上，每一档只留一句它自己的（"比 Q6_K 小一点"）。
 *
 * 「不下载」那一项没有家族，单独排在最后。
 */
function families(g) {
  const out = []
  const byName = new Map()
  for (const o of g.options) {
    if (!o.family) continue
    if (!byName.has(o.family)) {
      const fam = { name: o.family, note: o.familyNote, options: [] }
      byName.set(o.family, fam)
      out.push(fam)
    }
    byName.get(o.family).options.push(o)
  }
  return out
}

function looseOptions(g) {
  return g.options.filter((o) => !o.family)
}

/**
 * 家族下拉框里的选项。「不下载」当成一个只有一档的家族排在最后。
 *
 * **为什么是两个下拉框而不是一长串单选。** 一组十几到二十档
 * （Qwen-Image 15 档、Wan 14 档、编剧 20 档），全铺开的话设置页要滚
 * 十几屏，而用户真正要做的判断只有两步：先挑哪个模型，再挑哪一档精度。
 * 两个下拉框正好对上这两步。
 */
function familyChoices(g) {
  const out = families(g).map((f) => ({
    key: f.name,
    label: f.name,
    note: f.note,
    options: f.options,
  }))
  for (const o of looseOptions(g)) {
    out.push({ key: o.id, label: o.label, note: o.familyNote, options: [o] })
  }
  return out
}

/** 当前选中的是哪个家族。「不下载」用它自己的 id 当键。 */
function currentFamily(g) {
  const o = pickedOption(g)
  if (!o) return ''
  return o.family || o.id
}

function currentFamilyChoice(g) {
  return familyChoices(g).find((f) => f.key === currentFamily(g)) ?? null
}

/**
 * 换家族时替他挑一档。
 *
 * **不能留在原来那个 id 上**——那个 id 属于上一个家族，换完之后
 * 两个下拉框会对不上（家族显示新的、精度还是旧的）。
 *
 * 挑的顺序：推荐的那档如果就在这个家族里就用它；否则挑装得下的里面
 * 最好的一档；都装不下就挑最小的那档（列表是从大到小排的，取最后一个）。
 */
function selectFamily(g, key) {
  const fam = familyChoices(g).find((f) => f.key === key)
  if (!fam || !fam.options.length) return
  const rec = fam.options.find((o) => o.id === state.value?.recommended?.[g.key])
  const fits = fam.options.filter((o) => o.fits)
  picks.value[g.key] = (rec ?? fits[0] ?? fam.options[fam.options.length - 1]).id
}

/** 精度下拉框里一行怎么写。选项里没法排版，所以全塞进这一行字。 */
function optionLine(g, o) {
  const marks = []
  if (state.value?.selected?.[g.key] === o.id) marks.push('正在用')
  if (state.value?.recommended?.[g.key] === o.id) marks.push('推荐')
  if (o.complete) marks.push('已下好')
  const head = `${o.quant} · ${vramLabel(o)} 可常驻 · ${humanBytes(o.totalBytes)}`
  return marks.length ? `${head} · ${marks.join('、')}` : head
}

/** 「≥ 24 GB」这种角标。显存门槛说的是"权重能常驻"，不是"跑不起来"。 */
function vramLabel(o) {
  if (!o.minVramGb) return ''
  // 半档的写小数，整档不写——「≥ 33.5 GB」和「≥ 24 GB」都要好读
  const n = Number(o.minVramGb)
  return `≥ ${Number.isInteger(n) ? n : n.toFixed(1)} GB`
}

/** 选中的这一套一共多大、其中已经在盘上的有多少、这次要下多少。 */
const plan = computed(() => {
  let total = 0
  let have = 0
  for (const g of groups.value) {
    const opt = pickedOption(g)
    if (!opt || opt.id === 'none') continue
    total += opt.totalBytes
    have += opt.haveBytes
  }
  return { total, have, need: Math.max(0, total - have) }
})

/** 选的这一套和配置里现在用的那一套一样吗。设置页靠它决定按钮亮不亮。 */
const unchanged = computed(() => {
  const current = state.value?.selected ?? {}
  return groups.value.every((g) => (current[g.key] || '') === (picks.value[g.key] || ''))
})

const diskShort = computed(() => {
  const free = Number(state.value?.diskFreeBytes ?? 0)
  // 探不到空间（回 0）就不吓唬人。宁可让下载器自己报盘满，
  // 也别在盘明明够的时候拦着不让下。
  if (!free) return false
  return plan.value.need > free
})

const overallPercent = computed(() => {
  const p = progress.value
  if (!p || !p.total) return 0
  return Math.min(100, (Number(p.downloaded) / Number(p.total)) * 100)
})

const currentItem = computed(
  () => progress.value?.items?.find((i) => i.state === 'running') ?? null,
)

const failedItems = computed(
  () => progress.value?.items?.filter((i) => i.state === 'failed') ?? [],
)

function itemPercent(item) {
  if (!item.total) return 0
  return Math.min(100, (Number(item.downloaded) / Number(item.total)) * 100)
}

const ITEM_LABEL = {
  pending: '排队中',
  running: '正在下',
  done: '已完成',
  present: '盘上已有',
  failed: '失败',
  canceled: '已取消',
}

async function load() {
  loading.value = true
  loadError.value = ''
  try {
    const data = await api.setupState()
    state.value = data
    progress.value = data.download
    if (!dir.value) dir.value = data.modelsDir || ''
    // 已经配着的优先，没有就用按这张卡推出来的。**顺序不能反**——
    // 反过来的话，用户上次特意挑了小一档的模型，这一页会把它换回推荐的那档，
    // 而他多半不会注意到。
    picks.value = { ...data.recommended, ...pruneEmpty(data.selected) }
    if (!source.value) source.value = data.source || ''
    if (data.download?.state === 'running') startPolling()
  } catch (err) {
    loadError.value = err.message
  } finally {
    loading.value = false
  }
}

function pruneEmpty(obj) {
  const out = {}
  for (const [k, v] of Object.entries(obj || {})) if (v) out[k] = v
  return out
}

/** 一轮跑完（下完、失败、取消）之后的收尾。三条路都要走同一遍。 */
async function afterRun(kind) {
  const data = await api.setupState()
  state.value = data
  picks.value = { ...data.recommended, ...pruneEmpty(data.selected) }
  // **跑完一轮就算他在这一页上做过决定了。** 有几组故意选了「不下载」的话
  // 模型确实还缺，而不记这一笔的话他每次打开都会被拦回来，
  // 每次都要再点一次「先跳过」。
  markSetupHandled()
  // 路由守卫一个会话只问一次。不清掉的话，下完之后点「进入首页」
  // 会被那个缓存下来的"还缺模型"又弹回来。
  clearSetupCheck()
  emit('applied')
  if (kind !== 'done') return
  // **换一档已经下过的模型走的也是这条路**（引擎照样起一轮，只是每个文件
  // 都判成"盘上已有"）。那时候说"模型都准备好了"是答非所问——
  // 他刚做的事是换模型，不是下模型。
  const fetched = (progress.value?.items ?? []).some((i) => i.state === 'done')
  ui.ok(fetched ? '模型都准备好了' : '已经换过去了，配置写好了')
}

function startPolling() {
  if (timer) return
  timer = setInterval(async () => {
    try {
      const p = await api.setupProgress()
      progress.value = p
      if (p.state !== 'running') {
        stopPolling()
        await afterRun(p.state)
      }
    } catch {
      // 一次问不到不算事（引擎重启、网络抖一下），下一拍再问。
      // 停掉轮询的话进度条会永远冻在这一刻，而下载其实还在跑。
    }
  }, 1000)
}

function stopPolling() {
  if (timer) clearInterval(timer)
  timer = null
}

async function start() {
  starting.value = true
  try {
    const res = await api.startSetupDownload({
      selections: picks.value,
      dir: dir.value?.trim() || undefined,
      source: source.value || undefined,
    })
    progress.value = res.progress
    if (res.started) {
      startPolling()
      return
    }
    // 一个文件都不用碰（每一组选的都是「不下载」）。引擎那边连一轮都
    // 没起，所以这儿自己收尾。
    await afterRun('done')
  } catch (err) {
    ui.error(err.message)
  } finally {
    starting.value = false
  }
}

async function stop() {
  try {
    progress.value = await api.cancelSetupDownload()
  } catch (err) {
    ui.error(err.message)
  }
}

defineExpose({ reload: load, state, running })

onMounted(load)
onUnmounted(stopPolling)
</script>

<template>
  <div class="picker" :class="{ 'picker--dense': dense }">
    <p v-if="loading" class="tiny dim">正在读模型清单…</p>

    <div v-else-if="loadError" class="card card--bad">
      <div class="card__body stack stack--sm">
        <p class="strong">读不到模型清单</p>
        <p class="small dim">{{ loadError }}</p>
        <div><button class="btn btn--sm" type="button" @click="load">重试</button></div>
      </div>
    </div>

    <template v-else>
      <!-- ---------- 这台机器是什么样 ---------- -->
      <div class="facts">
        <div class="fact">
          <span class="fact__k tiny dim">显卡</span>
          <span class="fact__v">
            {{ state.gpu ? state.gpu.name : '没探测到' }}
            <span v-if="state.gpu" class="numeric dim">
              · {{ state.gpu.vramGb.toFixed(1) }} GB
              <template v-if="state.gpu.count > 1">× {{ state.gpu.count }}</template>
            </span>
          </span>
          <span v-if="!state.detected" class="tiny warnish">
            探不到，按 {{ state.vramGb.toFixed(0) }} GB 估
          </span>
        </div>
        <div class="fact">
          <span class="fact__k tiny dim">模型目录</span>
          <input
            v-model="dir"
            class="input input--path mono"
            :disabled="running"
            spellcheck="false"
            title="改成别的盘也行，写进配置的就是这个目录"
          />
        </div>
        <div class="fact">
          <span class="fact__k tiny dim">盘剩余</span>
          <span class="fact__v numeric">
            {{ state.diskFreeBytes ? humanBytes(state.diskFreeBytes) : '看不出来' }}
          </span>
        </div>
        <div class="fact">
          <span class="fact__k tiny dim">下载源</span>
          <select v-model="source" class="select select--src" :disabled="running">
            <option v-for="src in state.sources" :key="src.id" :value="src.id">
              {{ src.label }}{{ src.probe ? ` · ${src.probe}` : '' }}
            </option>
          </select>
          <span v-if="state.sourceHow === 'env'" class="tiny dim">
            被环境变量 CHANGJI_MODEL_SOURCE 顶着
          </span>
          <span v-else-if="state.sourceHow !== 'probed'" class="tiny dim">
            没探成（没有 curl），用的默认值
          </span>
        </div>
        <div class="fact">
          <span class="fact__k tiny dim">下载器</span>
          <span class="fact__v">
            <template v-if="state.tool === 'aria2c'">aria2c · 八连接</template>
            <template v-else-if="state.tool">{{ state.tool }} · 单连接</template>
            <template v-else>没找到</template>
          </span>
          <span v-if="state.tool === 'curl'" class="tiny warnish mono">
            单连接慢，装 aria2：apt-get install -y aria2 / winget install aria2.aria2
          </span>
          <span v-else-if="!state.tool" class="tiny warnish mono">
            装一个再回来：apt-get install -y aria2 / winget install aria2.aria2
          </span>
        </div>
      </div>

      <!-- ---------- 下载中 / 下完了 ---------- -->
      <div v-if="running || finished || failedItems.length" class="card card--run">
        <div class="card__body stack">
          <ProgressBar
            :percent="overallPercent"
            :tone="
              progress.state === 'failed'
                ? 'danger'
                : progress.state === 'done'
                  ? 'ok'
                  : 'accent'
            "
            :label="
              running
                ? currentItem
                  ? `正在下 ${currentItem.name}`
                  : '正在准备'
                : progress.state === 'done'
                  ? '全部就绪'
                  : progress.state === 'canceled'
                    ? '已停下'
                    : '有文件没下下来'
            "
            :detail="`${humanBytes(progress.downloaded)} / ${humanBytes(progress.total)}`"
          />
          <div class="row row--between tiny dim">
            <span class="numeric">
              <template v-if="progress.source">{{ progress.source }} · </template>
              <template v-if="running && progress.speedBps > 0">
                {{ humanRate(progress.speedBps) }}
                <template v-if="progress.etaSeconds > 0">
                  · 还要 {{ humanTime(progress.etaSeconds) }}
                </template>
              </template>
              <template v-else-if="running">正在连…</template>
            </span>
            <span>{{ overallPercent.toFixed(1) }}%</span>
          </div>

          <ul class="items">
            <li
              v-for="item in progress.items"
              :key="item.name"
              class="item"
              :class="`item--${item.state}`"
            >
              <span class="item__dot" />
              <span class="item__name mono truncate">{{ item.name }}</span>
              <span class="item__state tiny dim nowrap">
                {{ ITEM_LABEL[item.state] || item.state }}
              </span>
              <span class="item__size tiny dim numeric nowrap">
                <template v-if="item.state === 'running'">
                  {{ humanBytes(item.downloaded) }} / {{ humanBytes(item.total) }}
                  <template v-if="item.speedBps > 0">
                    · {{ humanRate(item.speedBps) }}
                  </template>
                </template>
                <template v-else>{{ humanBytes(item.total) }}</template>
              </span>
              <div v-if="item.state === 'running'" class="item__bar">
                <div class="item__fill" :style="{ width: itemPercent(item) + '%' }" />
              </div>
              <p v-if="item.error" class="item__err tiny">{{ item.error }}</p>
            </li>
          </ul>

          <p v-if="progress.error" class="small warnish">{{ progress.error }}</p>

          <div class="row">
            <button v-if="running" class="btn btn--danger btn--sm" type="button" @click="stop">
              <AppIcon name="stop" :size="14" />
              停下
            </button>
            <template v-else>
              <button
                v-if="failedItems.length || progress.state === 'canceled'"
                class="btn btn--sm"
                type="button"
                title="从断点续"
                @click="start"
              >
                <AppIcon name="refresh" :size="14" />
                接着下
              </button>
              <!-- 初始化页在这儿放「进入首页」；设置页什么都不放。 -->
              <slot name="done" :state="progress.state" />
            </template>
          </div>
        </div>
      </div>

      <!-- ---------- 选版本 ---------- -->
      <!-- 两个下拉框对上用户要做的两步判断：先挑哪个模型，再挑哪一档精度。
           家族那段话不摆出来，只留选中那一档自己的一句。 -->
      <section v-for="g in groups" :key="g.key" class="sec group">
        <div class="sec__head">
          <h3 class="sec__t" :title="g.purpose">{{ g.title }}</h3>
          <span v-if="!g.required" class="pill pill--neutral tiny">可选</span>
          <span v-if="g.satisfied" class="pill pill--ok tiny">已就绪</span>
        </div>
        <div class="stack stack--sm">
          <div class="pick">
            <label class="pick__field">
              <span class="field__label">模型</span>
              <select
                class="select"
                :disabled="running"
                :value="currentFamily(g)"
                @change="selectFamily(g, $event.target.value)"
              >
                <option v-for="fam in familyChoices(g)" :key="fam.key" :value="fam.key">
                  {{ fam.label }}
                </option>
              </select>
            </label>
            <label
              v-if="(currentFamilyChoice(g)?.options?.length ?? 0) > 1"
              class="pick__field"
            >
              <span class="field__label">
                精度
                <span class="tiny dim">{{ currentFamilyChoice(g).options.length }} 档</span>
              </span>
              <select v-model="picks[g.key]" class="select" :disabled="running">
                <option
                  v-for="o in currentFamilyChoice(g).options"
                  :key="o.id"
                  :value="o.id"
                >
                  {{ optionLine(g, o) }}
                </option>
              </select>
            </label>
          </div>

          <div v-if="pickedOption(g)" class="pick__row">
            <span v-if="state.selected[g.key] === pickedOption(g).id" class="pill pill--info tiny">
              正在用
            </span>
            <span
              v-if="state.recommended[g.key] === pickedOption(g).id"
              class="pill pill--accent tiny"
            >推荐</span>
            <span v-if="pickedOption(g).complete" class="pill pill--ok tiny">盘上已有</span>
            <!-- 显存门槛说的是"权重能常驻"，不是"跑不起来"。够不着是提醒，不是禁止。 -->
            <span
              v-if="pickedOption(g).minVramGb"
              class="pill tiny"
              :class="pickedOption(g).fits ? 'pill--ok' : 'pill--warn'"
            >
              {{ vramLabel(pickedOption(g)) }}
              {{ pickedOption(g).fits ? '常驻显存' : '放内存，慢' }}
            </span>
            <span class="spacer" />
            <span v-if="pickedOption(g).totalBytes" class="tiny dim numeric nowrap">
              {{ humanBytes(pickedOption(g).totalBytes) }}
            </span>
          </div>

          <!-- 选中那一档自己的一句。这是挑档的唯一依据，留着。 -->
          <p v-if="pickedOption(g)?.note" class="tiny dim">
            {{ pickedOption(g).note }}
          </p>

          <!-- 展开条件要带上"这一档有文件"，不然选「不下载」会留个空盒子。 -->
          <button
            v-if="pickedFiles(g).length > 0"
            class="btn btn--ghost btn--sm files__toggle"
            type="button"
            @click="expanded[g.key] = !expanded[g.key]"
          >
            {{ expanded[g.key] ? '收起' : '文件明细' }}
          </button>
          <ul v-if="expanded[g.key] && pickedFiles(g).length > 0" class="files">
            <li v-for="f in pickedFiles(g)" :key="f.name" class="file">
              <span class="file__name mono truncate" :title="f.note">{{ f.name }}</span>
              <span class="file__size numeric tiny dim nowrap">{{ humanBytes(f.bytes) }}</span>
              <span v-if="f.present" class="pill pill--ok tiny">已有</span>
            </li>
          </ul>
        </div>
      </section>

      <!-- ---------- 底下那条 ---------- -->
      <div class="foot">
        <div class="foot__sum">
          <span class="strong numeric">
            一共 {{ humanBytes(plan.total) }}
            <template v-if="plan.have > 0">
              <span class="dim">· 盘上已有 {{ humanBytes(plan.have) }}</span>
            </template>
          </span>
          <span class="tiny dim numeric">
            <template v-if="plan.need > 0">
              要下 {{ humanBytes(plan.need) }} · 约 {{ humanTime(plan.need / 10e6) }}，中途可停
            </template>
            <template v-else-if="unchanged">和现在用的一套一样</template>
            <template v-else>都在盘上，直接切</template>
          </span>
          <span v-if="diskShort" class="tiny bad">
            盘只剩 {{ humanBytes(state.diskFreeBytes) }}，装不下：换目录或挑小一档
          </span>
        </div>
        <div class="foot__act">
          <slot name="actions" :running="running" />
          <!-- 没改动时不禁用，只改文案：重写一遍配置是幂等的，配置漂了时靠它修。 -->
          <button
            class="btn btn--primary"
            type="button"
            :disabled="running || starting || !state.tool"
            :title="unchanged && plan.need === 0 ? '把这一套的配置项重写一遍，配置手改坏了时用' : ''"
            @click="start"
          >
            <AppIcon
              :name="plan.need > 0 ? 'upload' : 'check'"
              :size="16"
              :class="{ down: plan.need > 0 }"
            />
            {{
              starting
                ? '正在开始…'
                : plan.need > 0
                  ? '下载并使用'
                  : unchanged
                    ? '重写配置'
                    : '换成这一套'
            }}
          </button>
        </div>
      </div>

      <p v-if="!dense" class="tiny dim mono center">配置文件：{{ state.configFile }}</p>
    </template>
  </div>
</template>

<style scoped>
.picker {
  display: flex;
  flex-direction: column;
  gap: var(--s5);
}
.picker--dense {
  gap: var(--s4);
}

/* ---------- 机器情况 ---------- */

.facts {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: var(--s4);
}
.fact {
  display: flex;
  flex-direction: column;
  gap: 3px;
  min-width: 0;
}
.fact__v {
  font-size: var(--fs-base);
  font-weight: 500;
}
.input--path {
  height: 30px;
  padding: 0 8px;
  font-size: var(--fs-sm);
}
.warnish {
  color: var(--warn);
}
.bad {
  color: var(--danger);
}
.center {
  text-align: center;
}

/* ---------- 选项 ---------- */

.select--src {
  height: 30px;
  padding: 0 8px;
  font-size: var(--fs-sm);
}

/* ---------- 挑模型 ---------- */

.pick {
  display: grid;
  grid-template-columns: minmax(180px, 1fr) minmax(220px, 1.4fr);
  gap: var(--s3);
}
.pick__field {
  display: flex;
  flex-direction: column;
  gap: 4px;
  min-width: 0;
}
.pick__row {
  display: flex;
  align-items: center;
  gap: var(--s2);
  flex-wrap: wrap;
}

@media (max-width: 640px) {
  .pick {
    grid-template-columns: 1fr;
  }
}

.files__toggle {
  align-self: flex-start;
}
.files {
  display: flex;
  flex-direction: column;
  gap: var(--s2);
  padding: var(--s3);
  border-radius: var(--r-md);
  background: var(--surface-3);
}
.file {
  display: grid;
  grid-template-columns: 1fr auto auto;
  gap: var(--s2);
  align-items: center;
}

/* ---------- 下载进度 ---------- */

.card--run {
  border-color: var(--accent-line);
}
.card--bad {
  border-color: color-mix(in srgb, var(--danger) 40%, transparent);
}
.items {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.item {
  display: grid;
  grid-template-columns: 10px 1fr auto auto;
  align-items: center;
  gap: var(--s2);
}
.item__dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--text-3);
}
.item--running .item__dot {
  background: var(--accent);
}
.item--done .item__dot,
.item--present .item__dot {
  background: var(--ok);
}
.item--failed .item__dot {
  background: var(--danger);
}
.item--done .item__name,
.item--present .item__name {
  color: var(--text-2);
}
.item__bar {
  grid-column: 2 / -1;
  height: 3px;
  border-radius: var(--r-pill);
  background: var(--surface-3);
  overflow: hidden;
}
.item__fill {
  height: 100%;
  background: var(--accent);
  transition: width 0.4s var(--ease);
}
.item__err {
  grid-column: 2 / -1;
  color: var(--danger);
  white-space: pre-wrap;
  line-height: 1.6;
}

/* ---------- 底栏 ---------- */

/* 初始化页上它粘在底下，是真正浮着的东西，所以带底和边。 */
.foot {
  position: sticky;
  bottom: 0;
  display: flex;
  align-items: center;
  gap: var(--s4);
  flex-wrap: wrap;
  padding: var(--s4);
  border: 1px solid var(--line);
  border-radius: var(--r-lg);
  background: color-mix(in srgb, var(--surface) 94%, transparent);
  backdrop-filter: blur(12px);
  box-shadow: var(--shadow-1);
}
/* 设置页里不粘、不装盒：就是一行字和一个按钮。 */
.picker--dense .foot {
  position: static;
  padding: 0;
  border: none;
  background: none;
  backdrop-filter: none;
  box-shadow: none;
}
.foot__sum {
  flex: 1;
  min-width: 220px;
  display: flex;
  flex-direction: column;
  gap: 3px;
}
.foot__act {
  display: flex;
  gap: var(--s2);
  align-items: center;
}
/* 「上传」那个图标转过来当下载用。 */
.down {
  transform: rotate(180deg);
}

@media (max-width: 640px) {
  .foot__act {
    width: 100%;
  }
  .foot__act :deep(.btn),
  .foot__act .btn {
    flex: 1;
  }
}
</style>
