<script setup>
/**
 * 设置。
 *
 * 所有环境和参数都收在这一页，别的地方一个配置项都不放。
 * 分三层，按「改了会影响什么」排：
 *   连接——换机器；画质与装配——换成片规格；闸门——换废片判定。
 * 每一项都写清楚改了之后会发生什么，不然用户只敢照默认值跑。
 */
import { computed, onMounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import ModelPicker from '@/components/ModelPicker.vue'
import StepHeader from '@/components/StepHeader.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useUi } from '@/stores/ui'

const ui = useUi()
const { run, isBusy } = useAction()

const overview = ref(null)
const loading = ref(true)

const node = ref({ engineBaseUrl: '', engineTimeoutMs: 600000 })
const conn = ref({})
const params = ref({})
const persist = ref(true)
const apiKeyInput = ref('')

const SECTIONS = [
  { id: 'engine', title: '引擎', icon: 'link' },
  { id: 'llm', title: '大模型', icon: 'sparkle' },
  { id: 'models', title: '模型', icon: 'wand' },
  { id: 'render', title: '出图出片', icon: 'image' },
  { id: 'tts', title: '配音', icon: 'info' },
  { id: 'assembly', title: '成片装配', icon: 'board' },
  { id: 'gates', title: '质量闸门', icon: 'check' },
  { id: 'doctor', title: '体检', icon: 'warn' },
  { id: 'look', title: '外观', icon: 'moon' },
]

const engineOnline = computed(() => Boolean(overview.value?.engine?.online))

/**
 * 那台大模型服务上都有哪些模型。
 *
 * 模型名以前只能手打。打错了要跑到写剧本那一步才报错，而报出来的是一个
 * 404——分不清是地址错了还是名字错了。拉过来给人选，这类错就没机会发生。
 */
const models = ref([])
const modelsError = ref('')
const modelsLoading = ref(false)

const modelMissing = computed(
  () =>
    models.value.length > 0 &&
    conn.value.llm_model &&
    !models.value.includes(conn.value.llm_model),
)

/**
 * 拉模型列表。
 *
 * 拉到之后，如果当前填的模型这台服务上没有（或者压根没填），就默认选第一个。
 * 不这么做的话，换完平台地址那一刻配置是坏的——地址是新平台的，模型名还是
 * 上一家的，点保存就存进去一个跑不通的组合。
 */
async function loadModels({ pickFirst = false } = {}) {
  modelsLoading.value = true
  modelsError.value = ''
  try {
    const data = await api.llmModels()
    models.value = data.models ?? []
    modelsError.value = data.error ?? ''
    const current = conn.value.llm_model
    if (models.value.length && (pickFirst || !current || !models.value.includes(current))) {
      conn.value.llm_model = models.value[0]
    }
  } catch (err) {
    modelsError.value = err.message
  } finally {
    modelsLoading.value = false
  }
}

/**
 * 平台预设。
 *
 * 各家都是 OpenAI 兼容接口，差别只在 base_url 和密钥，所以不用为每一家写
 * 适配器——列出来只是免得用户去翻各家文档找那一行地址。选完仍然能手改。
 *
 * 选中一家之后要立刻把地址存进引擎再拉模型列表：模型列表是引擎按它当前
 * 的配置去问的，不先存就还是在问上一家。
 */
const providers = ref([])
const providerId = ref('')

async function loadProviders() {
  try {
    providers.value = (await api.llmProviders()).providers ?? []
  } catch {
    providers.value = []
  }
}

const currentProvider = computed(
  () => providers.value.find((p) => p.id === providerId.value) ?? null,
)

// 地址和某一家对上了就把选择器显示成那一家，没对上就是「自定义」
watch(
  () => [conn.value.llm_base_url, providers.value.length],
  () => {
    const url = String(conn.value.llm_base_url || '').replace(/\/+$/, '')
    const hit = providers.value.find(
      (p) => p.base_url.replace(/\/+$/, '') === url,
    )
    providerId.value = hit?.id ?? ''
  },
)

async function pickProvider(event) {
  const id = event.target.value
  providerId.value = id
  const provider = providers.value.find((p) => p.id === id)
  if (!provider) return
  conn.value.llm_base_url = provider.base_url
  // 先把地址落到引擎上，模型列表才问得对地方
  const saved = await run(
    () =>
      api.saveConnections({
        patch: { llm_base_url: provider.base_url },
        persist: persist.value,
      }),
    { key: 'provider', quiet: true },
  )
  if (!saved) {
    ui.warn('地址没存上，模型列表可能还是上一家的')
    return
  }
  await loadModels({ pickFirst: true })
  if (models.value.length) {
    ui.ok(`已切到 ${provider.name}，模型默认选了 ${models.value[0]}`)
  } else if (provider.local) {
    ui.warn(`${provider.name} 那边没应答。服务起了吗？`)
  } else {
    ui.info(`${provider.name} 的地址填好了，还要在下面填 API Key 才能问到模型列表`)
  }
}
const envLocked = computed(() => conn.value?.env_locked ?? {})
const nodeLocked = computed(() => node.value?.envLocked ?? {})

/**
 * 引擎是不是就是发这个页面的那个进程。
 *
 * 是的话「引擎地址」和「请求超时」两个输入框没有意义：它们是给 Node 那层
 * 转发用的，由引擎自己答时是空的，改了也没人读。显示出来只会让人以为
 * 哪里没配好——地址栏空着、旁边还挂个"已连接 0ms"。
 */
const embedded = computed(() => node.value?.embedded === true)

/**
 * 大模型跑在哪：`local`（进程内）还是 `remote`（走 base_url）。
 *
 * 走 local 时下面那一整排远端字段——平台、API 地址、模型名、密钥——
 * **一个都不读**。摆着只会让人调了没反应，和之前那个"配音引擎"输入框
 * 一样。权重路径在配置文件的 [models].llm。
 *
 * 这一项从 bff 拿，不从 /api/connections：那个接口在对拍覆盖范围内，
 * Python 没有这个字段，加进去就是一处破契约。
 */
const llmLocal = computed(() => node.value?.llmBackend === 'local')

/** 内置和外接之间切。切完重读一遍——体检那几项会跟着变。 */
async function switchLlm(e) {
  const backend = e.target.value
  const ok = await run(() => api.saveLlmBackend(backend), {
    key: 'llmBackend',
    success: backend === 'local' ? '大模型改成内置' : '大模型改成外接 API',
  })
  // 失败时把下拉框拨回去——不拨的话它显示的是没生效的那个值，
  // 而用户会以为已经切过去了。
  e.target.value = ok ? backend : node.value?.llmBackend || 'local'
  if (ok) await load()
}
const hardware = computed(() => overview.value?.hardware)

/**
 * 这一轮**真正会用**的步数和画幅。
 *
 * **别拿 hardware.tiers.final 显示。** 那是档位表按显存推出来的，出片时
 * 会被两件事盖掉：画幅来自项目的 [video]，步数在挂了 Turbo 时压到 6。
 * 照着档位表显示的话，界面写"成片步数 28"而每一镜实际跑 6 步——
 * 用户看了会问"怎么没用 turbo"。真发生过（2026-09-10）。
 *
 * 引擎那边和 run.cpp 调的是同一个函数（config::effective_spec），
 * 两边不会分叉。
 */
const effective = computed(() => overview.value?.effective ?? null)

// 程序算出来的权重放置。两个模型各一行；没有这一项（老引擎）就整块不显示。
const placement = computed(() => effective.value?.placement ?? null)
const placementRows = computed(() => {
  const p = placement.value
  if (!p) return []
  return [
    { key: 'image', label: '出首帧', ...(p.image ?? {}) },
    { key: 'video', label: '出片', ...(p.video ?? {}) },
  ].filter((r) => r.weights !== undefined)
})

async function load() {
  loading.value = true
  try {
    const data = await api.settingsOverview()
    overview.value = data
    node.value = { ...data.node }
    conn.value = { ...(data.connections ?? {}) }
    // 引擎把参数分了组，界面上摊平成一层，提交时再拆回去
    const s = data.settings
    if (s) {
      // **档位那六项不进来。** 「画质档位」那一节 2026-09-10 删了
      // （画幅搬到项目上），但字段当时还留在这儿——界面上没有控件，
      // 值却照样跟着「保存参数」写回配置文件。后果不是多存几行：
      // `[tiers].final_steps` 一旦有值，出片时的 Turbo 6 步就不再生效
      // （run.cpp 只在它是 0 时才动），每一镜悄悄变回 28 步、慢四倍，
      // 而界面上只说了一句"参数已保存到配置文件"。
      params.value = {
        ...s.assembly,
        ...s.gates,
        tts_tolerance_s: s.tts?.tolerance_s,
        tts_max_tempo_shift: s.tts?.max_tempo_shift,
      }
    }
    for (const [key, message] of Object.entries(data.errors ?? {})) {
      ui.warn(`${key} 读不到：${message}`)
    }
    if (data.engine?.online) loadModels()
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  load()
  loadProviders()
})

async function saveNode() {
  const result = await run(() => api.saveNodeConfig(node.value), {
    key: 'node',
    success: '引擎地址已保存',
  })
  if (result) {
    node.value = { ...result }
    await load()
  }
}

/** 连接类设置。改了等于换一台干活的机器，保存完引擎会自动重新体检。 */
async function saveConnections() {
  const patch = {
    llm_base_url: conn.value.llm_base_url,
    llm_model: conn.value.llm_model,
    llm_temperature: Number(conn.value.llm_temperature),
    tts_backend: conn.value.tts_backend,
    tts_base_url: conn.value.tts_base_url ?? '',
    tts_engine: conn.value.tts_engine,
  }
  // 密钥只在用户真填了新的时候才提交。空着就是「别动它」，
  // 提交空串会把原来的密钥抹掉。
  if (apiKeyInput.value.trim()) patch.llm_api_key = apiKeyInput.value.trim()

  const result = await run(() => api.saveConnections({ patch, persist: persist.value }), {
    key: 'conn',
  })
  if (!result) return
  apiKeyInput.value = ''
  ui.ok(
    result.changed?.length
      ? `改了 ${result.labels?.join('、') || result.changed.join('、')}`
      : '没有要改的项',
  )
  if (result.env_locked?.length) {
    ui.warn(`${result.env_locked.join('、')} 被环境变量顶着，重启还是环境变量那一套`)
  }
  await load()
}

async function saveParams() {
  const patch = {}
  for (const [k, v] of Object.entries(params.value)) {
    if (v === '' || v === null || v === undefined) continue
    patch[k] = typeof v === 'boolean' ? v : Number.isNaN(Number(v)) ? v : Number(v)
  }
  // 布尔项被上面的 Number 转换弄坏过一次，这里显式拨回来
  patch.gates_enabled = Boolean(params.value.enabled ?? params.value.gates_enabled)
  patch.fallback_on_exhausted = Boolean(params.value.fallback_on_exhausted)
  delete patch.enabled

  const result = await run(() => api.saveEngineSettings({ patch, persist: persist.value }), {
    key: 'params',
    success: persist.value ? '参数已保存到配置文件' : '参数已生效（重启后失效）',
  })
  if (result) await load()
}

function scrollTo(id) {
  document.getElementById('sec-' + id)?.scrollIntoView({ behavior: 'smooth', block: 'start' })
}
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader title="设置" tagline="环境、地址和参数全在这一页。别处不放配置项。">
      <template #actions>
        <button class="btn btn--ghost" type="button" :disabled="loading" @click="load">
          <AppIcon name="refresh" :size="15" :class="{ spin: loading }" />
          重新读取
        </button>
      </template>
    </StepHeader>

    <div class="set">
      <!-- 小节导航 -->
      <nav class="secnav">
        <button
          v-for="s in SECTIONS"
          :key="s.id"
          class="secnav__item"
          type="button"
          @click="scrollTo(s.id)"
        >
          <AppIcon :name="s.icon" :size="14" />
          {{ s.title }}
        </button>
        <label class="secnav__persist">
          <input v-model="persist" type="checkbox" />
          <span>写回配置文件</span>
        </label>
      </nav>

      <div class="stack stack--lg set__body">
        <!-- 引擎 -->
        <section id="sec-engine" class="card">
          <div class="card__head">
            <div>
              <div class="card__title">引擎</div>
              <div class="card__sub">
                {{ embedded
                  ? '这个页面就是引擎自己发的，同一个进程，没有别的服务要配。'
                  : '真正干活的服务。可以在本机，也可以在局域网另一台机器上。' }}
              </div>
            </div>
            <span class="pill" :class="engineOnline ? 'pill--ok' : 'pill--danger'">
              {{ engineOnline ? `已连接 ${overview?.engine?.latencyMs}ms` : '连不上' }}
            </span>
          </div>
          <div class="card__body stack">
            <p v-if="!engineOnline" class="alert alert--bad">
              <AppIcon name="warn" :size="15" />
              {{ overview?.engine?.error || '引擎离线，下面的设置读不到也存不了。' }}
            </p>

            <div v-if="!embedded" class="grid grid--2">
              <label class="field">
                <span class="field__label">
                  引擎地址
                  <span v-if="nodeLocked.engineBaseUrl" class="pill pill--warn tiny">
                    被 {{ nodeLocked.engineBaseUrl }} 顶着
                  </span>
                </span>
                <input v-model="node.engineBaseUrl" class="input mono" placeholder="http://127.0.0.1:8000" />
                <span class="field__hint">changji web 跑起来之后监听的地址。</span>
              </label>
              <label class="field">
                <span class="field__label">请求超时（毫秒）</span>
                <input v-model.number="node.engineTimeoutMs" class="input numeric" type="number" step="1000" />
                <span class="field__hint">出分镜这类活儿要几分钟，别调太小。</span>
              </label>
            </div>
            <!-- 自带引擎时，把这张卡片换成有用的东西：跑在什么机器上、
                 配置从哪读的。地址和超时那两栏在这种情况下是空的。 -->
            <!-- **成片步数不在这儿显示。** 下面「出图出片」那张卡上有同一个
                 数、同一句说明；一字不差地重复两遍只是让这一页更长。
                 这里只留跟"跑在哪台机器上"有关的：显卡，和配置文件路径。 -->
            <!-- **显存显示的是探到的那个数，不是配置里顶着的。**
                 /api/hardware 的 vram_gb 在有 vram_gb_override 时回的是 override，
                 这里以前照着显示，于是写着 "5090 · 12 GB"——用户报的
                 "硬件 GPU 显存有获取不准的 bug"。真实显存从 effective 拿。 -->
            <div v-if="embedded" class="field">
              <span class="field__label">显卡</span>
              <span class="mono">
                {{ hardware?.gpu || '未探测到显卡' }}
                <template v-if="effective?.physicalVramGb">
                  · {{ Number(effective.physicalVramGb).toFixed(1) }} GB
                </template>
              </span>
              <span v-if="effective?.vramOverride" class="field__hint warn-text">
                配置里 vram_gb_override = {{ effective.vramOverride }} 顶着这张卡，
                档位表按它推。程序会自己算，把这一行从配置文件里删掉。
              </span>
            </div>
            <!-- **程序给两个模型算出来的权重放置。**
                 weights 不让人在这儿填（"都应该让程序自己算"），但算完了
                 不给看是另一个极端：出图慢到底是卡不行、还是权重在内存里
                 每步搬一趟，用户没有线索。排查 GPU 利用率 18% 那次，
                 答案就是这一项，当时得连上机器看日志才知道。 -->
            <div v-if="embedded && placement" class="field">
              <span class="field__label">权重放哪（程序自己算的）</span>
              <div class="stack">
                <div v-for="row in placementRows" :key="row.key" class="mono tiny">
                  {{ row.label }}：
                  <span :class="row.resident ? 'pill pill--ok tiny' : 'pill pill--warn tiny'">
                    {{ row.resident ? '常驻显存' : '权重放内存' }}
                  </span>
                  <template v-if="row.modelGb > 0">
                    模型 {{ row.modelGb.toFixed(1) }} GB，跑起来约占
                    {{ row.liveVramGb.toFixed(1) }} GB
                  </template>
                  <template v-else>（模型没配或读不到文件）</template>
                </div>
              </div>
              <span class="field__hint">
                按这张卡的显存和模型文件大小算出来的。**权重放内存不等于出了问题**——
                装不下时放内存反而更快，显卡腾出来的地方全给了计算。
              </span>
            </div>
            <p class="tiny dim mono">配置文件：{{ node.configFile }}</p>
          </div>
          <div v-if="!embedded" class="card__foot">
            <button class="btn btn--primary" type="button" :disabled="isBusy('node')" @click="saveNode">
              保存引擎地址
            </button>
          </div>
        </section>

        <template v-if="engineOnline && conn.llm_base_url !== undefined">
          <!-- 大模型 -->
          <section id="sec-llm" class="card">
            <div class="card__head">
              <div>
                <div class="card__title">大模型</div>
                <div class="card__sub">
                  {{ llmLocal
                    ? '写剧本和出分镜用它。当前是进程内跑，不用另起服务——权重在配置文件的 [models].llm。'
                    : '写剧本和出分镜用它。任何兼容 OpenAI 接口的服务都行。' }}
                </div>
              </div>
            </div>
            <div class="card__body">
              <label class="field">
                <span class="field__label">跑在哪</span>
                <select
                  class="select"
                  :value="node?.llmBackend || 'local'"
                  :disabled="isBusy('llmBackend')"
                  @change="switchLlm"
                >
                  <option value="local">内置（这个进程里跑，不用装别的）</option>
                  <option value="remote">外接 API（任何兼容 OpenAI 接口的服务）</option>
                </select>
                <span class="field__hint">
                  {{ llmLocal
                    ? '权重在配置文件的 [models].llm。出片要显存时它会自动让开。'
                    : '下面填那个服务的地址和模型名。' }}
                </span>
              </label>
            </div>
            <!-- 走内置时这一整排都不读，直接不显示。压暗过一版，
                 但"看得见却不生效"仍然要人自己判断，不如不给。 -->
            <div v-if="!llmLocal" class="card__body grid grid--2">
              <label class="field field--wide">
                <span class="field__label">平台</span>
                <select
                  class="select"
                  :value="providerId"
                  :disabled="isBusy('provider')"
                  @change="pickProvider"
                >
                  <option value="">自定义 / 已填的地址</option>
                  <optgroup label="本机跑的">
                    <option
                      v-for="p in providers.filter((x) => x.local)"
                      :key="p.id"
                      :value="p.id"
                    >
                      {{ p.name }}
                    </option>
                  </optgroup>
                  <optgroup label="云服务（要密钥）">
                    <option
                      v-for="p in providers.filter((x) => !x.local)"
                      :key="p.id"
                      :value="p.id"
                    >
                      {{ p.name }}
                    </option>
                  </optgroup>
                </select>
                <span v-if="currentProvider" class="field__hint">
                  {{ currentProvider.note }}
                </span>
                <span v-else class="field__hint">
                  这些平台都是 OpenAI 兼容接口，选一个会自动填地址并挑好模型；
                  也可以直接在下面手填。
                </span>
              </label>

              <label class="field">
                <span class="field__label">
                  API 地址
                  <span v-if="envLocked.llm_base_url" class="pill pill--warn tiny">
                    被 {{ envLocked.llm_base_url }} 顶着
                  </span>
                </span>
                <input v-model="conn.llm_base_url" class="input mono" placeholder="http://127.0.0.1:11434/v1" />
                <span class="field__hint">要带 /v1。云服务填它的兼容地址即可。</span>
              </label>
              <label class="field">
                <span class="field__label">
                  模型名
                  <button
                    class="linkbtn tiny"
                    type="button"
                    :disabled="modelsLoading"
                    @click.prevent="loadModels"
                  >
                    {{ modelsLoading ? '正在问…' : '重新拉列表' }}
                  </button>
                </span>
                <input
                  v-model="conn.llm_model"
                  class="input mono"
                  list="llm-models"
                  placeholder="qwen3:14b"
                />
                <datalist id="llm-models">
                  <option v-for="m in models" :key="m" :value="m" />
                </datalist>
                <span v-if="modelMissing" class="field__error">
                  这台服务上没有 {{ conn.llm_model }}。它有的是：{{ models.join('、') }}。
                  写剧本那一步会直接失败，换一个或者先把它拉下来。
                </span>
                <span v-else-if="modelsError" class="field__hint">
                  列不出有哪些模型（{{ modelsError }}），手打也行。
                </span>
                <span v-else-if="models.length" class="field__hint">
                  这台服务上有 {{ models.length }} 个模型，点输入框能选。
                </span>
              </label>
              <label class="field">
                <span class="field__label">
                  API Key
                  <span class="pill pill--neutral tiny">
                    {{ conn.llm_api_key_set ? conn.llm_api_key_hint : '未设置' }}
                  </span>
                </span>
                <input
                  v-model="apiKeyInput"
                  class="input mono"
                  type="password"
                  placeholder="留空表示不改"
                  autocomplete="off"
                />
                <span class="field__hint">本地服务通常不校验，填什么都行。</span>
              </label>
              <label class="field">
                <span class="field__label">温度</span>
                <input
                  v-model.number="conn.llm_temperature"
                  class="input numeric"
                  type="number"
                  step="0.1"
                  min="0"
                  max="2"
                />
                <span class="field__hint">高了更敢编，低了更听话。写剧本 0.7 上下。</span>
              </label>
            </div>
          </section>

          <!-- 模型 -->
          <!--
            **和初始化页是同一个组件**（ModelPicker）。分两份写的话，量化档的
            说明、显存门槛、"换家族要清空上一家的键"这些迟早只改一边，
            而分家的表现是两页各写一套，用户不知道该信哪个。

            这一节做两件事：把缺的模型下下来，和**在已经下过的几档之间切换**。
            后一件以前只能改配置文件——而配置里那十来个键（video / video_llm /
            video_vae / video_audio_vae / video_lora…）必须整组配套换，
            漏一个不报错，只是出一段和提示词没关系的片。
          -->
          <section id="sec-models" class="card">
            <div class="card__head">
              <div>
                <div class="card__title">模型</div>
                <div class="card__sub">
                  下新模型，或者在已经下过的几档之间换。换完立刻写进配置，
                  下一次出片就用新的。
                </div>
              </div>
            </div>
            <div class="card__body">
              <ModelPicker dense @applied="load" />
            </div>
          </section>

          <!-- 出图出片 -->
          <section id="sec-render" class="card">
            <div class="card__head">
              <div>
                <div class="card__title">出图出片</div>
                <div class="card__sub">
                  首帧和视频都在这个进程里跑，没有外部服务要配。
                  模型文件在配置文件的 [models] 一节。
                </div>
              </div>
              <!-- 模型本身在上面那一节挑。这儿只放个指路的，
                   免得看完步数和画幅之后还要满页找在哪儿换模型。 -->
              <button class="btn btn--sm" type="button" @click="scrollTo('models')">
                <AppIcon name="wand" :size="14" />
                换模型 / 下模型
              </button>
            </div>
            <div class="card__body grid grid--2">
              <!-- **「显存覆盖」那个输入框删了（2026-09-10）。**
                   它做的事是让档位表按一个假的显存数推，而这个假数还会被
                   /api/hardware 当成探测结果回出去——设置页上写着
                   "5090 · 12 GB"。画幅早就是项目自己的了，步数由 Turbo 定，
                   它剩下的用途只有制造误会。用户的原话："都应该让程序自己算。"
                   配置文件里还认这个键（老配置不报错），但界面上不再给。 -->
              <!-- **显示的是真正会用的数，不是档位表推的。**
                   档位表那份（宽高和步数）出片时会被两件事盖掉：画幅来自
                   项目的 [video]，步数在挂了 Turbo 时压到 6。照着档位表显示
                   的话，这儿写"成片步数 28"而每一镜实际跑 6 步——
                   用户看了会问"怎么没用 turbo"。真发生过（2026-09-10）。
                   引擎那边和 run.cpp 算的是同一个函数，见 effective。 -->
              <div class="field">
                <span class="field__label">步数</span>
                <span class="mono">
                  出片 {{ effective?.finalSteps ?? '—' }}
                  <span v-if="effective?.turbo" class="pill pill--ok tiny">Turbo</span>
                  <span v-else-if="effective?.stepsPinned" class="pill pill--neutral tiny">
                    配置里写死
                  </span>
                  · 首帧 {{ effective?.frameSteps ?? '—' }}
                </span>
                <span v-if="effective?.turbo" class="field__hint">
                  挂着 Turbo LoRA，出视频按 6 步走（档位表推的是
                  {{ effective?.tableSteps }} 步）。首帧不跟着变——那个 LoRA
                  只挂在视频模型上，出图那一步没有它，跟着降到 6 步会让首帧糊，
                  而首帧是后面每一镜的锚点。
                </span>
                <span v-else class="field__hint">
                  档位表按显存推的。想提速就在 [models].video_lora 填一个
                  蒸馏 LoRA，出视频会自动按 6 步走。
                </span>
              </div>
              <div class="field">
                <span class="field__label">成片画幅</span>
                <span class="mono">
                  {{ effective ? `${effective.width}×${effective.height}` : '—' }}
                </span>
                <span class="field__hint">
                  每部剧自己的，在项目页的「画面」那张卡选。
                </span>
              </div>
            </div>
          </section>

          <!-- 配音 -->
          <section id="sec-tts" class="card">
            <div class="card__head">
              <div>
                <div class="card__title">配音</div>
                <div class="card__sub">先跑配音拿到真实时长，再反推锁定镜头时长。音画从源头对齐。</div>
              </div>
            </div>
            <div class="card__body grid grid--2">
              <label class="field">
                <span class="field__label">后端</span>
                <select v-model="conn.tts_backend" class="select">
                  <option value="local">进程内跑（不用装别的东西）</option>
                  <option value="http">独立 HTTP 服务</option>
                </select>
              </label>
              <label class="field">
                <span class="field__label">服务地址</span>
                <input
                  v-model="conn.tts_base_url"
                  class="input mono"
                  :disabled="conn.tts_backend !== 'http'"
                  placeholder="后端选 http 时必填"
                />
              </label>
              <!-- **[tts].engine 那一项没了。** 它是 ComfyUI 时代的字段
                   （选哪个 TTS 节点），而两条现存后端都不读它：进程内跑的是
                   [models].tts 指的那份权重，HTTP 后端发出去的请求体里根本
                   没有这个字段（Python 那版也一样，两边一致）。
                   配置和接口里保留是为了不破契约，提交时原样带回去；
                   界面上摆着只会让人调了没反应——那比没有这一项更糟。 -->
              <label class="field">
                <span class="field__label">时长容差（秒）</span>
                <input
                  v-model.number="params.tts_tolerance_s"
                  class="input numeric"
                  type="number"
                  step="0.05"
                  min="0"
                />
                <span class="field__hint">台词和镜头时长的允许偏差，超出靠尾帧冻结或变速吸收。</span>
              </label>
            </div>
            <div class="card__foot">
              <button
                class="btn btn--primary"
                type="button"
                :disabled="isBusy('conn')"
                @click="saveConnections"
              >
                {{ isBusy('conn') ? '保存中…' : '保存连接设置并重新体检' }}
              </button>
              <!-- 大模型走内置时上面一个大模型字段都没显示，
                   还说"大模型和配音后端一起保存"就是在说一件没发生的事。 -->
              <span class="tiny dim">
                {{ llmLocal ? '保存配音后端。' : '大模型和配音后端一起保存。' }}
              </span>
            </div>
          </section>

          <!-- **「画质档位」那一节删了（2026-09-10）。**

               画幅和清晰度搬到项目上了（项目页的「画面」卡片），
               因为一台机器上可以同时有竖屏短剧和横屏片子。
               搬完之后这里改宽高**不再生效**——出片时项目的 [video]
               会盖掉它，而界面照旧显示"已应用"。

               步数还有意义（[tiers].final_steps，0 = 挂了 Turbo 就按
               6 走），但那是专家旋钮，留在配置文件里。 -->

          <!-- 装配 -->
          <section id="sec-assembly" class="card">
            <div class="card__head">
              <div>
                <div class="card__title">成片装配</div>
                <div class="card__sub">拼接环节最容易踩的坑是各镜头规格不齐，这里统一规格。</div>
              </div>
            </div>
            <div class="card__body grid grid--3">
              <label class="field">
                <span class="field__label">帧率</span>
                <input v-model.number="params.fps" class="input numeric" type="number" />
              </label>
              <label class="field">
                <span class="field__label">CRF</span>
                <input v-model.number="params.crf" class="input numeric" type="number" min="0" max="51" />
                <span class="field__hint">数字越小画质越好、文件越大。18 是常用值。</span>
              </label>
              <label class="field">
                <span class="field__label">场景转场（秒）</span>
                <input
                  v-model.number="params.scene_transition_s"
                  class="input numeric"
                  type="number"
                  step="0.1"
                />
                <span class="field__hint">只在换场景处溶解，同场景内一律硬切。</span>
              </label>
              <label class="field">
                <span class="field__label">字幕字体</span>
                <input v-model="params.subtitle_font" class="input" />
              </label>
              <label class="field">
                <span class="field__label">单行字数</span>
                <input
                  v-model.number="params.subtitle_max_chars_per_line"
                  class="input numeric"
                  type="number"
                />
              </label>
              <label class="field">
                <span class="field__label">最多几行</span>
                <input v-model.number="params.subtitle_max_lines" class="input numeric" type="number" />
              </label>
            </div>
          </section>

          <!-- 闸门 -->
          <section id="sec-gates" class="card">
            <div class="card__head">
              <div>
                <div class="card__title">质量闸门</div>
                <div class="card__sub">
                  无人值守量产时，这几个数字决定废片能不能被拦住。
                </div>
              </div>
              <label class="switch">
                <input v-model="params.enabled" type="checkbox" />
                <span>开启</span>
              </label>
            </div>
            <div class="card__body grid grid--3">
              <label class="field">
                <span class="field__label">每镜最多重试</span>
                <input
                  v-model.number="params.max_attempts_per_shot"
                  class="input numeric"
                  type="number"
                  min="1"
                />
              </label>
              <label class="field">
                <span class="field__label">画面标准差下限</span>
                <input v-model.number="params.min_pixel_std" class="input numeric" type="number" step="0.5" />
                <span class="field__hint">拦纯色和噪点。调高会误杀暗场。</span>
              </label>
              <label class="field">
                <span class="field__label">与首帧相似度下限</span>
                <input
                  v-model.number="params.min_frame_similarity"
                  class="input numeric"
                  type="number"
                  step="0.05"
                  min="0"
                  max="1"
                />
                <span class="field__hint">拦画面跑飞。运动大的片子要调低。</span>
              </label>
              <label class="field">
                <span class="field__label">台词落点偏差上限（秒）</span>
                <input
                  v-model.number="params.max_audio_drift_s"
                  class="input numeric"
                  type="number"
                  step="0.05"
                />
              </label>
              <label class="field">
                <span class="field__label">响度目标（LUFS）</span>
                <input v-model.number="params.target_lufs" class="input numeric" type="number" step="0.5" />
                <span class="field__hint">短视频平台一般收 -16 到 -14。</span>
              </label>
              <label class="field">
                <span class="field__label">音频变速安全区</span>
                <input
                  v-model.number="params.tts_max_tempo_shift"
                  class="input numeric"
                  type="number"
                  step="0.01"
                  min="0"
                  max="0.2"
                />
                <span class="field__hint">有口型的镜头收得更紧，超了会听出来。</span>
              </label>
            </div>
            <div class="card__body" style="padding-top: 0">
              <label class="switch">
                <input v-model="params.fallback_on_exhausted" type="checkbox" />
                <span>重试超限时降级为静帧加运镜</span>
                <span class="field__hint">保证整集能出片，而不是卡在某一镜上。</span>
              </label>
            </div>
            <div class="card__foot">
              <button
                class="btn btn--primary"
                type="button"
                :disabled="isBusy('params')"
                @click="saveParams"
              >
                {{ isBusy('params') ? '保存中…' : '保存装配与闸门参数' }}
              </button>
              <span class="tiny dim">
                {{ persist ? '会写回配置文件，重启还在。' : '只对本次进程生效，重启就没了。' }}
              </span>
            </div>
          </section>

          <!-- 体检 -->
          <section id="sec-doctor" class="card">
            <div class="card__head">
              <div>
                <div class="card__title">体检</div>
                <!-- 别再写"三样缺一不可"：大模型现在是黄字不是红字——
                     出片那条路不用它，分镜表也可以手写。说成必需的，
                     用户会为了一条不挡出片的警告卡在这儿。 -->
                <div class="card__sub">
                  红的必须先解决，黄的是"这一块还用不了"，其余照跑。
                </div>
              </div>
              <span
                v-if="overview?.doctor"
                class="pill"
                :class="overview.doctor.can_run ? 'pill--ok' : 'pill--danger'"
              >
                {{ overview.doctor.can_run ? '可以开工' : '还不能跑' }}
              </span>
            </div>
            <div v-if="overview?.doctor" class="card__body stack stack--sm">
              <div
                v-for="c in overview.doctor.checks"
                :key="c.name"
                class="check"
                :class="`check--${c.level}`"
              >
                <AppIcon :name="c.level === 'ok' ? 'check' : 'warn'" :size="14" />
                <span class="check__name nowrap">{{ c.name }}</span>
                <span class="check__detail">{{ c.detail }}</span>
                <span v-if="c.fix" class="check__fix tiny dim">{{ c.fix }}</span>
              </div>
            </div>
          </section>
        </template>

        <!-- 外观 -->
        <section id="sec-look" class="card">
          <div class="card__head">
            <div>
              <div class="card__title">外观</div>
              <div class="card__sub">深色是默认。白天在办公室改分镜可以切浅色。</div>
            </div>
          </div>
          <div class="card__body">
            <div class="chips">
              <button
                v-for="t in [
                  { v: 'system', l: '跟随系统' },
                  { v: 'dark', l: '深色' },
                  { v: 'light', l: '浅色' },
                ]"
                :key="t.v"
                class="chip"
                :class="{ 'chip--on': ui.theme === t.v }"
                type="button"
                @click="ui.theme = t.v"
              >
                {{ t.l }}
              </button>
            </div>
          </div>
        </section>
      </div>
    </div>
  </div>
</template>

<style scoped>
.set {
  display: grid;
  grid-template-columns: 168px minmax(0, 1fr);
  gap: var(--s6);
  align-items: start;
}

.secnav {
  position: sticky;
  top: var(--s4);
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.secnav__item {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s2) var(--s3);
  border: none;
  border-radius: var(--r);
  background: none;
  color: var(--text-2);
  font-size: var(--fs-base);
  cursor: pointer;
  text-align: left;
}
.secnav__item:hover {
  background: var(--surface-2);
  color: var(--text);
}
.secnav__persist {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin-top: var(--s3);
  padding: var(--s3);
  border-radius: var(--r);
  border: 1px solid var(--line);
  font-size: var(--fs-sm);
  color: var(--text-2);
  cursor: pointer;
}
.secnav__persist input {
  accent-color: var(--accent);
  width: 15px;
  height: 15px;
}

.grid--2 {
  grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
}
/* 平台选择器独占一行。它是这一节的入口——选完地址和模型都跟着变，
   跟旁边那些手填的框挤在一起就看不出先后。 */
.field--wide {
  grid-column: 1 / -1;
  max-width: 420px;
}
.grid--3 {
  grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
}

/* 标签栏里那种「重新拉一次」的小动作。做成按钮但长得像链接，
   免得和字段旁边真正的操作按钮混在一起。 */
.linkbtn {
  border: none;
  background: none;
  padding: 0;
  color: var(--accent);
  cursor: pointer;
  font-weight: 500;
}
.linkbtn:hover:not(:disabled) {
  text-decoration: underline;
}
.linkbtn:disabled {
  color: var(--text-3);
  cursor: default;
}

.tierbox {
  padding: var(--s4);
  border-radius: var(--r);
  border: 1px solid var(--line);
  background: var(--bg-sunken);
}
.tierbox__head {
  font-size: var(--fs-sm);
  font-weight: 700;
  color: var(--accent);
  margin-bottom: var(--s3);
}

.alert {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
  padding: var(--s3);
  border-radius: var(--r);
  font-size: var(--fs-base);
  line-height: 1.6;
}
.alert--bad {
  background: var(--danger-soft);
  color: var(--danger);
}

.check {
  display: flex;
  align-items: baseline;
  gap: var(--s2);
  padding: var(--s2) var(--s3);
  border-radius: var(--r-sm);
  background: var(--bg-sunken);
  font-size: var(--fs-sm);
  line-height: 1.5;
  flex-wrap: wrap;
}
.check :deep(svg) {
  align-self: center;
  color: var(--text-3);
}
.check--ok :deep(svg) {
  color: var(--ok);
}
.check--warn {
  background: var(--warn-soft);
}
.check--warn :deep(svg) {
  color: var(--warn);
}
.check--error {
  background: var(--danger-soft);
}
.check--error :deep(svg) {
  color: var(--danger);
}
.check__name {
  font-weight: 600;
  width: 7em;
}
.check__detail {
  flex: 1;
  min-width: 12ch;
}

.switch {
  display: inline-grid;
  grid-template-columns: auto 1fr;
  align-items: center;
  gap: var(--s2);
  cursor: pointer;
  font-size: var(--fs-base);
}
.switch input {
  width: 16px;
  height: 16px;
  accent-color: var(--accent);
}
.switch .field__hint {
  grid-column: 2;
  margin-top: -4px;
}

.chips {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}
.chip {
  padding: 4px var(--s3);
  border-radius: var(--r-pill);
  border: 1px solid var(--line);
  background: var(--surface-2);
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.chip--on {
  background: var(--accent-soft);
  border-color: var(--accent-line);
  color: var(--accent);
  font-weight: 600;
}

.spin {
  animation: spin 0.9s linear infinite;
}
@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}

@media (max-width: 900px) {
  .set {
    grid-template-columns: 1fr;
  }
  .secnav {
    position: static;
    flex-direction: row;
    flex-wrap: nowrap;
    overflow-x: auto;
    gap: var(--s2);
    padding-bottom: var(--s2);
    scrollbar-width: none;
  }
  .secnav::-webkit-scrollbar {
    display: none;
  }
  .secnav__item {
    white-space: nowrap;
    border: 1px solid var(--line);
  }
  .secnav__persist {
    margin-top: 0;
    white-space: nowrap;
  }
}
/* 配置里有 vram_gb_override 顶着真实显存时那句提醒 */
.warn-text {
  color: var(--warn);
}

/* 「上传」那个图标转过来当下载用。为一个箭头再画一个图标不值当。 */
.down {
  transform: rotate(180deg);
}
</style>
