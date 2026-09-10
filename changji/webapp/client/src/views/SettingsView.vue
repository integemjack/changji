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
    vram_gb_override: conn.value.vram_gb_override
      ? Number(conn.value.vram_gb_override)
      : null,
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
            <div v-if="embedded" class="grid grid--2">
              <div class="field">
                <span class="field__label">显卡</span>
                <span class="mono">
                  {{ hardware?.gpu || '未探测到显卡' }}
                  <template v-if="hardware?.vram_gb"> · {{ hardware.vram_gb }} GB</template>
                </span>
              </div>
              <!-- **只显示步数，不显示宽高。**
                   宽高由项目的 [video] 说了算，而这里读的是全局档位表——
                   两个数不一样时（很常见：全局 960×544、项目 704×1280）
                   显示出来只会让人以为设置没生效。 -->
              <div class="field">
                <span class="field__label">成片步数</span>
                <span class="mono">
                  {{ hardware?.tiers?.final?.steps ?? '—' }}
                </span>
                <span class="field__hint">
                  画幅和清晰度是每部剧自己的，在项目页的「画面」那张卡选。
                </span>
              </div>
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
            </div>
            <div class="card__body grid grid--2">
              <label class="field">
                <span class="field__label">显存覆盖（GB）</span>
                <input
                  v-model="conn.vram_gb_override"
                  class="input numeric"
                  type="number"
                  step="1"
                  placeholder="留空表示自动探测"
                />
                <span class="field__hint">
                  只影响档位表怎么推（步数那些），不是"这张卡有多少显存"。
                  画幅和清晰度在项目页上选。
                  想要更高的成片档就把它调大。
                </span>
              </label>
              <div class="field">
                <span class="field__label">当前档位</span>
                <span class="mono">
                  <template v-if="hardware?.tiers?.final">
                    成片 {{ hardware.tiers.final.width }}×{{ hardware.tiers.final.height }}
                    / {{ hardware.tiers.final.steps }} 步
                  </template>
                  <template v-else>—</template>
                </span>
                <span class="field__hint">在项目页的「画面」那张卡改。</span>
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
              <span class="tiny dim">大模型和配音后端一起保存。</span>
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
                <div class="card__sub">模型文件、大模型、FFmpeg 三样缺一不可。</div>
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
</style>
