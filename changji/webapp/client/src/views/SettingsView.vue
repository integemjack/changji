<script setup>
/**
 * 设置。
 *
 * 所有环境和参数都收在这一页，别的地方一个配置项都不放。
 * 分三层，按「改了会影响什么」排：
 *   连接——换机器；画质与装配——换成片规格；闸门——换废片判定。
 * 页面上只放控件和读数。一个控件要是非得解释才会用，解释进它的 title。
 */
import { computed, onMounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import ModelPicker from '@/components/ModelPicker.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useUi } from '@/stores/ui'
import { describeRoomDecision } from '@/composables/room-decision'
import { describeLlmState } from '@/composables/llm-state'
import { placementRows as buildPlacementRows } from '@/composables/placement-rows'

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
  { id: 'engine', title: '引擎' },
  { id: 'llm', title: '大模型' },
  { id: 'models', title: '模型' },
  { id: 'render', title: '出图出片' },
  { id: 'tts', title: '配音' },
  { id: 'assembly', title: '装配' },
  { id: 'gates', title: '闸门' },
  { id: 'doctor', title: '体检' },
  { id: 'look', title: '外观' },
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

/**
 * 大模型现在装着没有。
 *
 * 「默认加载 llm，点击出片清理掉大模型，够就不清理」——这三句描述的都是
 * 同一个状态，而它以前在界面上完全看不到：用户只能看到"跑在哪"是内置，
 * 看不出此刻权重是在显存里还是已经被出片腾走了。
 *
 * 是页面打开那一刻的快照，刷新才更新——够用了：用户是在出片前后各看一眼
 * 来确认"到底清没清"。
 */
const llmState = computed(() => describeLlmState(overview.value?.node))

// 程序算出来的权重放置。两个模型各一行；没有这一项（老引擎）就整块不显示。
const placement = computed(() => effective.value?.placement ?? null)
const llmRuntime = computed(() => effective.value?.llm ?? null)
const placementRows = computed(() => buildPlacementRows(placement.value))

/**
 * 最近一次「要不要腾地方」的判断，翻成人话。
 *
 * 这个判断错了的表现是"该留的时候卸了"（出片前白等几十秒重装大模型）
 * 或者"该卸的时候没卸"（显存爆掉，整个服务没了）。以前只能登上机器看
 * stderr——服务器连不上的时候这条线索就断了。摆在这儿，谁都看得见。
 */
const roomDecision = computed(() =>
  describeRoomDecision(placement.value?.lastRoomDecision),
)

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
    <div class="toolbar">
      <span class="spacer" />
      <button class="btn btn--ghost btn--sm" type="button" :disabled="loading" @click="load">
        <AppIcon name="refresh" :size="14" :class="{ spin: loading }" />
        重新读取
      </button>
    </div>

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
          {{ s.title }}
        </button>
        <label
          class="secnav__persist"
          title="勾上：改动写回配置文件，重启还在。不勾：只对本次进程生效，重启就没了"
        >
          <input v-model="persist" type="checkbox" />
          <span>写回配置文件</span>
        </label>
      </nav>

      <div class="stack stack--lg set__body">
        <!-- 引擎 -->
        <section id="sec-engine" class="sec">
          <div class="sec__head">
            <h2 class="sec__t">引擎</h2>
            <span class="pill" :class="engineOnline ? 'pill--ok' : 'pill--danger'">
              {{ engineOnline ? `已连接 ${overview?.engine?.latencyMs}ms` : '连不上' }}
            </span>
            <span class="spacer" />
            <div v-if="!embedded" class="sec__acts">
              <button
                class="btn btn--primary btn--sm"
                type="button"
                :disabled="isBusy('node')"
                @click="saveNode"
              >
                保存
              </button>
            </div>
          </div>
          <div class="stack">
            <p v-if="!engineOnline" class="alert alert--bad">
              <AppIcon name="warn" :size="15" />
              {{ overview?.engine?.error || '引擎离线' }}
            </p>

            <div v-if="!embedded" class="grid grid--2">
              <label class="field">
                <span class="field__label">
                  引擎地址
                  <span v-if="nodeLocked.engineBaseUrl" class="pill pill--warn tiny">
                    被 {{ nodeLocked.engineBaseUrl }} 顶着
                  </span>
                </span>
                <input
                  v-model="node.engineBaseUrl"
                  class="input mono"
                  placeholder="http://127.0.0.1:8000"
                  title="changji web 跑起来之后监听的地址"
                />
              </label>
              <label class="field">
                <span class="field__label">超时（毫秒）</span>
                <input
                  v-model.number="node.engineTimeoutMs"
                  class="input numeric"
                  type="number"
                  step="1000"
                  title="出分镜要几分钟，别调太小"
                />
              </label>
            </div>
            <!-- 自带引擎时，地址和超时那两栏是空的，换成跑在什么机器上。
                 显存显示探到的那个数（effective），不是配置里顶着的。 -->
            <div v-if="embedded" class="field">
              <span class="field__label">显卡</span>
              <span class="mono">
                {{ hardware?.gpu || '未探测到显卡' }}
                <template v-if="effective?.physicalVramGb">
                  · {{ Number(effective.physicalVramGb).toFixed(1) }} GB
                </template>
              </span>
              <span v-if="effective?.vramOverride" class="tiny warn-text">
                配置里 vram_gb_override = {{ effective.vramOverride }} 顶着这张卡，把那一行删掉
              </span>
            </div>
            <!-- 配的上限和实际开出来的要分开摆：真开几个由显存说了算。 -->
            <div
              v-if="embedded && llmRuntime && llmRuntime.backend === 'local'"
              class="field"
            >
              <span class="field__label">大模型并行</span>
              <div class="mono tiny">
                <template v-if="!llmRuntime.loaded">
                  还没装上 · 上限 {{ llmRuntime.parallelWanted }} 路
                </template>
                <template v-else>
                  <span
                    class="pill tiny"
                    :class="
                      llmRuntime.slots >= llmRuntime.parallelWanted
                        ? 'pill--ok'
                        : 'pill--warn'
                    "
                    :title="
                      llmRuntime.slots < llmRuntime.parallelWanted
                        ? '显存只够开这么多'
                        : ''
                    "
                  >
                    {{ llmRuntime.slots }} 路
                  </span>
                  上限 {{ llmRuntime.parallelWanted }} 路 · 每路
                  {{ llmRuntime.contextTokens }} token
                </template>
              </div>
            </div>

            <!-- 程序给两个模型算出来的权重放置。「够就不清理」判的是实测那个数。 -->
            <div v-if="embedded && placement" class="field">
              <span class="field__label">权重放哪</span>
              <div class="stack stack--sm">
                <div v-for="row in placementRows" :key="row.key" class="mono tiny">
                  {{ row.label }}：
                  <span :class="row.resident ? 'pill pill--ok tiny' : 'pill pill--warn tiny'">
                    {{ row.resident ? '常驻显存' : '放内存' }}
                  </span>
                  <template v-if="row.modelGb > 0">
                    模型 {{ row.modelGb.toFixed(1) }} GB · 估 {{ row.liveVramGb.toFixed(1) }} GB<template
                      v-if="row.measured !== null"
                    > · <strong>实测 {{ row.measured.toFixed(1) }} GB</strong><template
                      v-if="row.measuredWorkMp !== null"
                    >（{{ row.measuredWorkMp }} MP·帧）</template></template>
                  </template>
                  <template v-else>（模型没配或读不到）</template>
                </div>
              </div>
            </div>
            <!-- 最近一次腾地方的判断。没发生过就不显示。 -->
            <div v-if="embedded && roomDecision" class="field">
              <span class="field__label">上次腾显存</span>
              <div v-if="roomDecision.skipped" class="mono tiny">
                借「{{ roomDecision.slot }}」：{{ roomDecision.verdict }}
              </div>
              <div v-else class="mono tiny">
                借「{{ roomDecision.slot }}」：需要 {{ roomDecision.need }}（{{ roomDecision.needHow }}）·
                空闲 {{ roomDecision.free }}（{{ roomDecision.how }}）
                <span :class="roomDecision.ok ? 'pill pill--ok tiny' : 'pill pill--warn tiny'">
                  {{ roomDecision.verdict }}
                </span>
              </div>
              <!-- 判「够」却是拿估的判的：这正是 CUDA OOM 的前一步。 -->
              <span
                v-if="!roomDecision.skipped && roomDecision.ok && !roomDecision.needTrusted"
                class="tiny warn-text"
              >
                这次的「够」是估算判的，这个槽还没量过，真跑可能不够
              </span>
            </div>
            <p class="tiny dim mono">配置文件：{{ node.configFile }}</p>
          </div>
        </section>

        <template v-if="engineOnline && conn.llm_base_url !== undefined">
          <!-- 大模型 -->
          <section id="sec-llm" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">大模型</h2>
              <!-- 「出片时自动让开」是句空话，除非能看到让没让开。 -->
              <span v-if="llmLocal" class="pill pill--neutral tiny mono">
                {{ llmState.text }}<template v-if="llmState.measured">
                  · 实测 {{ llmState.measured }}</template>
              </span>
            </div>
            <div class="stack">
              <label class="field field--narrow">
                <span class="field__label">跑在哪</span>
                <select
                  class="select"
                  :value="node?.llmBackend || 'local'"
                  :disabled="isBusy('llmBackend')"
                  title="内置：这个进程里跑，权重在配置文件的 [models].llm。外接：任何兼容 OpenAI 接口的服务"
                  @change="switchLlm"
                >
                  <option value="local">内置</option>
                  <option value="remote">外接 API</option>
                </select>
              </label>
              <!-- 走内置时这一整排都不读，直接不显示。 -->
              <div v-if="!llmLocal" class="grid grid--2">
                <label class="field field--wide">
                  <span class="field__label">平台</span>
                  <select
                    class="select"
                    :value="providerId"
                    :disabled="isBusy('provider')"
                    :title="currentProvider?.note || '选一个会自动填地址并挑好模型，也可以在下面手填'"
                    @change="pickProvider"
                  >
                    <option value="">自定义</option>
                    <optgroup label="本机">
                      <option
                        v-for="p in providers.filter((x) => x.local)"
                        :key="p.id"
                        :value="p.id"
                      >
                        {{ p.name }}
                      </option>
                    </optgroup>
                    <optgroup label="云服务">
                      <option
                        v-for="p in providers.filter((x) => !x.local)"
                        :key="p.id"
                        :value="p.id"
                      >
                        {{ p.name }}
                      </option>
                    </optgroup>
                  </select>
                </label>

                <label class="field">
                  <span class="field__label">
                    API 地址
                    <span v-if="envLocked.llm_base_url" class="pill pill--warn tiny">
                      被 {{ envLocked.llm_base_url }} 顶着
                    </span>
                  </span>
                  <input
                    v-model="conn.llm_base_url"
                    class="input mono"
                    placeholder="http://127.0.0.1:11434/v1"
                    title="要带 /v1"
                  />
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
                    这台服务上没有 {{ conn.llm_model }}，有的是：{{ models.join('、') }}
                  </span>
                  <span v-else-if="modelsError" class="tiny warn-text">
                    列不出模型：{{ modelsError }}
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
                    title="本地服务通常不校验"
                  />
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
                    title="高了更敢编，低了更听话。写剧本 0.7 上下"
                  />
                </label>
              </div>
            </div>
          </section>

          <!-- 模型：和初始化页是同一个组件（ModelPicker）。 -->
          <section id="sec-models" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">模型</h2>
            </div>
            <ModelPicker dense @applied="load" />
          </section>

          <!-- 出图出片 -->
          <!-- 显示的是真正会用的数（effective），不是档位表推的：
               画幅来自项目的 [video]，步数在挂了 Turbo 时压到 6。 -->
          <section id="sec-render" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">出图出片</h2>
              <span class="spacer" />
              <div class="sec__acts">
                <button class="btn btn--ghost btn--sm" type="button" @click="scrollTo('models')">
                  换模型
                </button>
              </div>
            </div>
            <div class="grid grid--2">
              <div class="field">
                <span class="field__label">步数</span>
                <span class="mono">
                  出片 {{ effective?.finalSteps ?? '—' }}
                  <span
                    v-if="effective?.turbo"
                    class="pill pill--ok tiny"
                    :title="`挂着 Turbo LoRA，出视频按 6 步走（档位表推的是 ${effective?.tableSteps} 步）`"
                  >Turbo</span>
                  <span v-else-if="effective?.stepsPinned" class="pill pill--neutral tiny">
                    配置里写死
                  </span>
                  · 首帧 {{ effective?.frameSteps ?? '—' }}
                </span>
              </div>
              <div class="field">
                <span class="field__label">画幅</span>
                <span class="mono" title="每部剧自己的，在项目页上选">
                  {{ effective ? `${effective.width}×${effective.height}` : '—' }}
                </span>
              </div>
            </div>
          </section>

          <!-- 配音 -->
          <section id="sec-tts" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">配音</h2>
              <span class="spacer" />
              <div class="sec__acts">
                <!-- 大模型走内置时上面一个大模型字段都没显示，那时只保存配音。 -->
                <button
                  class="btn btn--primary btn--sm"
                  type="button"
                  :disabled="isBusy('conn')"
                  :title="llmLocal ? '保存配音后端，保存完重新体检' : '大模型和配音一起保存，保存完重新体检'"
                  @click="saveConnections"
                >
                  {{ isBusy('conn') ? '保存中…' : '保存' }}
                </button>
              </div>
            </div>
            <div class="grid grid--2">
              <label class="field">
                <span class="field__label">后端</span>
                <select v-model="conn.tts_backend" class="select">
                  <option value="local">内置</option>
                  <option value="http">HTTP 服务</option>
                </select>
              </label>
              <label class="field">
                <span class="field__label">服务地址</span>
                <input
                  v-model="conn.tts_base_url"
                  class="input mono"
                  :disabled="conn.tts_backend !== 'http'"
                  placeholder="后端选 HTTP 时必填"
                />
              </label>
              <!-- [tts].engine 不在界面上：两条后端都不读它，提交时原样带回去。 -->
              <label class="field">
                <span class="field__label">时长容差（秒）</span>
                <input
                  v-model.number="params.tts_tolerance_s"
                  class="input numeric"
                  type="number"
                  step="0.05"
                  min="0"
                  title="台词和镜头时长的允许偏差，超出靠尾帧冻结或变速吸收"
                />
              </label>
            </div>
          </section>

          <!-- 装配 -->
          <section id="sec-assembly" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">装配</h2>
            </div>
            <div class="grid grid--3">
              <label class="field">
                <span class="field__label">帧率</span>
                <input v-model.number="params.fps" class="input numeric" type="number" />
              </label>
              <label class="field">
                <span class="field__label">CRF</span>
                <input
                  v-model.number="params.crf"
                  class="input numeric"
                  type="number"
                  min="0"
                  max="51"
                  title="越小画质越好、文件越大。18 是常用值"
                />
              </label>
              <label class="field">
                <span class="field__label">转场（秒）</span>
                <input
                  v-model.number="params.scene_transition_s"
                  class="input numeric"
                  type="number"
                  step="0.1"
                  title="只在换场景处溶解，同场景内一律硬切"
                />
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
          <section id="sec-gates" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">闸门</h2>
              <label class="switch">
                <input v-model="params.enabled" type="checkbox" />
                <span>开启</span>
              </label>
              <span class="spacer" />
              <div class="sec__acts">
                <button
                  class="btn btn--primary btn--sm"
                  type="button"
                  :disabled="isBusy('params')"
                  title="装配和闸门一起保存"
                  @click="saveParams"
                >
                  {{ isBusy('params') ? '保存中…' : '保存' }}
                </button>
              </div>
            </div>
            <div class="stack">
              <div class="grid grid--3">
                <label class="field">
                  <span class="field__label">每镜重试</span>
                  <input
                    v-model.number="params.max_attempts_per_shot"
                    class="input numeric"
                    type="number"
                    min="1"
                  />
                </label>
                <label class="field">
                  <span class="field__label">标准差下限</span>
                  <input
                    v-model.number="params.min_pixel_std"
                    class="input numeric"
                    type="number"
                    step="0.5"
                    title="拦纯色和噪点。调高会误杀暗场"
                  />
                </label>
                <label class="field">
                  <span class="field__label">首帧相似度下限</span>
                  <input
                    v-model.number="params.min_frame_similarity"
                    class="input numeric"
                    type="number"
                    step="0.05"
                    min="0"
                    max="1"
                    title="拦画面跑飞。运动大的片子要调低"
                  />
                </label>
                <label class="field">
                  <span class="field__label">台词偏差上限（秒）</span>
                  <input
                    v-model.number="params.max_audio_drift_s"
                    class="input numeric"
                    type="number"
                    step="0.05"
                  />
                </label>
                <label class="field">
                  <span class="field__label">响度（LUFS）</span>
                  <input
                    v-model.number="params.target_lufs"
                    class="input numeric"
                    type="number"
                    step="0.5"
                    title="短视频平台一般收 -16 到 -14"
                  />
                </label>
                <label class="field">
                  <span class="field__label">变速安全区</span>
                  <input
                    v-model.number="params.tts_max_tempo_shift"
                    class="input numeric"
                    type="number"
                    step="0.01"
                    min="0"
                    max="0.2"
                    title="有口型的镜头收得更紧，超了会听出来"
                  />
                </label>
              </div>
              <label class="switch" title="保证整集能出片，而不是卡在某一镜上">
                <input v-model="params.fallback_on_exhausted" type="checkbox" />
                <span>重试超限降级为静帧加运镜</span>
              </label>
            </div>
          </section>

          <!-- 体检。大模型是黄字不是红字：出片那条路不用它。 -->
          <section id="sec-doctor" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">体检</h2>
              <span
                v-if="overview?.doctor"
                class="pill"
                :class="overview.doctor.can_run ? 'pill--ok' : 'pill--danger'"
              >
                {{ overview.doctor.can_run ? '可以开工' : '还不能跑' }}
              </span>
            </div>
            <div v-if="overview?.doctor" class="stack stack--sm">
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
        <section id="sec-look" class="sec">
          <div class="sec__head">
            <h2 class="sec__t">外观</h2>
          </div>
          <div class="row">
            <button
              v-for="t in [
                { v: 'system', l: '跟随系统' },
                { v: 'dark', l: '深色' },
                { v: 'light', l: '浅色' },
              ]"
              :key="t.v"
              class="btn btn--ghost btn--sm"
              :class="{ 'is-on': ui.theme === t.v }"
              type="button"
              @click="ui.theme = t.v"
            >
              {{ t.l }}
            </button>
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

/* 小节导航：一列安静的字，不做成按钮。 */
.secnav {
  position: sticky;
  top: var(--s4);
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.secnav__item {
  padding: 3px 0;
  border: none;
  background: none;
  color: var(--text-3);
  font-size: var(--fs-sm);
  cursor: pointer;
  text-align: left;
}
.secnav__item:hover {
  color: var(--text);
}
.secnav__persist {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin-top: var(--s3);
  font-size: var(--fs-xs);
  color: var(--text-3);
  cursor: pointer;
}
.secnav__persist input {
  accent-color: var(--accent);
  width: 14px;
  height: 14px;
}

.grid--2 {
  grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
}
/* 平台选择器独占一行：选完地址和模型都跟着变。 */
.field--wide {
  grid-column: 1 / -1;
  max-width: 420px;
}
.field--narrow {
  max-width: 230px;
}
.grid--3 {
  grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
}

/* 标签栏里那种「重新拉一次」的小动作。做成按钮但长得像链接。 */
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
  display: inline-flex;
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

/* 外观那三个：选中的那个点亮。 */
.is-on {
  color: var(--accent);
  background: var(--accent-soft);
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
    gap: var(--s3);
    padding-bottom: var(--s2);
    scrollbar-width: none;
  }
  .secnav::-webkit-scrollbar {
    display: none;
  }
  .secnav__item {
    white-space: nowrap;
  }
  .secnav__persist {
    margin-top: 0;
    white-space: nowrap;
  }
}
/* 配置里有 vram_gb_override 顶着真实显存、估算判「够」那类提醒 */
.warn-text {
  color: var(--warn);
}
</style>
