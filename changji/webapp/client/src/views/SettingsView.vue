<script setup>
/**
 * 设置。
 *
 * 所有环境和参数都收在这一页，别的地方一个配置项都不放。
 * 页面上只放控件和读数。一个控件要是非得解释才会用，解释进它的 title。
 *
 * 2026-09-14 重排。**打开这一页要回答的问题只有一个：能不能跑、不能的话
 * 卡在哪。** 答案是体检，而它原来排在 1200px 的表单后面、12 项全印
 * （包括 sd.cpp 的 `SSE3 = 1 | AVX = 1 …` 那种调试串）。现在体检在最上面，
 * 只摆没过的，全部折在「▸ 另外 N 项」里。
 *
 * 砍掉的：
 *   · 「出图出片」——两个只读数加一个去项目页的链接。画幅是项目的，
 *     换模型在项目页，步数是算出来的诊断，进体检一行。
 *   · 「外观」——顶栏那个月亮图标就是主题切换，这一节是它的第二份。
 *   · 引擎里的「权重放哪」「上次腾显存」——运行诊断，不是设置，归体检；
 *     显卡、画幅在引擎和体检里各说了一遍，留体检那份。
 *   · 装配里的「转场（秒）」——assemble.cpp 自己写着"xfade / acrossfade /
 *     fade= 都没有，转场从来没被渲染过"。能改、存得住、什么都不做，和
 *     09-13 删的「时长容差」同一类。
 * 装配和闸门合成一节：它们本来就是一个按钮一起存的。
 */
import { computed, onMounted, ref } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useUi } from '@/stores/ui'
import { describeRoomDecision } from '@/composables/room-decision'
import { placementRows as buildPlacementRows } from '@/composables/placement-rows'

const ui = useUi()
const { run, isBusy } = useAction()

const overview = ref(null)
const loading = ref(true)

const node = ref({ engineBaseUrl: '', engineTimeoutMs: 600000 })
const conn = ref({})
const params = ref({})
const persist = ref(true)

// 模型往哪儿下。整台机器一套，和"挑哪个模型"（在项目页）是两件事。
const modelsDir = ref('')
const modelsSource = ref('')
const modelsSources = ref([])
const modelsTool = ref('')
const savedDir = ref('')
const savedSource = ref('')
const modelsDirty = computed(
  () => modelsDir.value !== savedDir.value || modelsSource.value !== savedSource.value,
)

async function loadModelsDir() {
  try {
    const d = await api.setupState()
    modelsDir.value = d.modelsDir || ''
    modelsSource.value = d.source || ''
    modelsSources.value = d.sources || []
    modelsTool.value = d.tool || ''
    savedDir.value = modelsDir.value
    savedSource.value = modelsSource.value
  } catch {
    // 读不到就留空。这一节不该让整页红——它装机时配一次，平时不看。
  }
}

async function saveModelsDir() {
  // **一组模型都不选**：这一节只管往哪儿下，不管下什么。引擎那边
  // `download: false` 加空 selections 就是"只存设置"（见 post_setup_download）。
  const ok = await run(
    () =>
      api.startSetupDownload({
        selections: {},
        dir: modelsDir.value.trim() || undefined,
        source: modelsSource.value || undefined,
        download: false,
      }),
    { key: 'modelsDir', success: '存好了' },
  )
  if (!ok) return
  await loadModelsDir()
}

const SECTIONS = [
  { id: 'doctor', title: '体检' },
  { id: 'look', title: '界面' },
  { id: 'engine', title: '引擎' },
  // 「大模型」那一节 2026-09-14 整个删了（用户：「加上 key，去掉设置里的
  // 大模型选择」）。服务、模型名、接口地址、密钥、温度全在项目页点模型名
  // 弹出来的那个窗口里——一件事分两页配，改完一处另一处还显示着旧的。
  // 「挑哪个模型」2026-09-14 搬去项目页了（用户的话："去掉设置页面的
  // 模型选择，改放进项目页面"）。**搬走的是"挑"，不是"往哪儿下"**——
  // 目录和下载源是整台机器共用的（"模型的路径放到设置里，这个全局统一的"），
  // 留在这儿。
  { id: 'tts', title: '配音' },
  { id: 'models', title: '模型目录和下载' },
  { id: 'assembly', title: '装配与闸门' },
]

const engineOnline = computed(() => Boolean(overview.value?.engine?.online))
/**
 * 体检里没过的那几项。
 *
 * **level 只有三个值：`ok` / `warn` / `fail`**（doctor.hpp 的 to_string
 * 写死的，那儿还专门写着"前端按这三个值上色"）。这行注释原来写的是
 * "warn / bad"、底下样式表里写的是 `check--error`——同一档东西三处三个
 * 名字，而三个里没有一个是线上真发的那个。见下面 .check--fail 那段。
 */
const failedChecks = computed(() =>
  (overview.value?.doctor?.checks ?? []).filter((c) => c.level !== 'ok'),
)
const okChecks = computed(() =>
  (overview.value?.doctor?.checks ?? []).filter((c) => c.level === 'ok'),
)

/**
 * 两节各自要提交的那几项，和"读回来之后动过没有"。
 *
 * 摆出「未存」那个角标是有用的：这一页上每一节的保存都是独立的，
 * 改了一节去点另一节的保存按钮，什么都不会发生，而页面上原来没有任何
 * 迹象说明这件事。
 */
const llmPatch = computed(() => ({
  llm_base_url: conn.value.llm_base_url,
  llm_model: conn.value.llm_model,
  llm_temperature: Number(conn.value.llm_temperature),
}))
const ttsPatch = computed(() => ({
  tts_backend: conn.value.tts_backend,
  tts_base_url: conn.value.tts_base_url ?? '',
}))
const savedLlm = ref('')
const savedTts = ref('')
const ttsDirty = computed(() => JSON.stringify(ttsPatch.value) !== savedTts.value)

const nodeLocked = computed(() => node.value?.envLocked ?? {})

/**
 * 引擎是不是就是发这个页面的那个进程。
 *
 * 是的话「引擎地址」和「请求超时」两个输入框没有意义：它们是给 Node 那层
 * 转发用的，由引擎自己答时是空的，改了也没人读。显示出来只会让人以为
 * 哪里没配好——地址栏空着、旁边还挂个"已连接 0ms"。
 */
const embedded = computed(() => node.value?.embedded === true)

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

const placement = computed(() => effective.value?.placement ?? null)
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
    // 基准线：这两句之后「未存」才说得准。放在这儿而不是保存成功那一下，
    // 是因为引擎可能没照单全收（被环境变量顶着的项就不会变）。
    savedLlm.value = JSON.stringify(llmPatch.value)
    savedTts.value = JSON.stringify(ttsPatch.value)
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
      }
    }
    for (const [key, message] of Object.entries(data.errors ?? {})) {
      ui.warn(`${key} 读不到：${message}`)
    }
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  loadModelsDir()
  load()
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
/**
 * 连接类设置。改了等于换一台干活的机器，保存完引擎会自动重新体检。
 *
 * **一节一个按钮，各存各的。** 2026-09-14 之前是一个按钮把大模型和配音
 * 一起提交，而那个按钮长在「配音」那一节里——用户在「大模型」挑完模型，
 * 整节上没有一个能点的东西，只能靠标题里那句"大模型和配音一起保存"猜。
 * 分开之后还有一个好处：改配音不会把大模型那几项也重写一遍。
 *
 * 密钥仍然不在这儿提交，它有自己的按钮（saveApiKey）——捎带着提交的话，
 * "改个温度"会顺手把密钥也写一遍。
 */
async function saveConn(patch, key) {
  const result = await run(() => api.saveConnections({ patch, persist: persist.value }), {
    key,
  })
  if (!result) return
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

/** 「配音」那一节。`tts_engine` 不提交——引擎的白名单里没有这一项，
 *  而 /api/connections 也从来不回它，提交的是个 undefined。 */
const saveTts = () => saveConn(ttsPatch.value, 'conn')

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
        <!-- 体检。**排最上面，只摆没过的。** 这一页要回答的就是"能不能跑、
             卡在哪"；过了的那十几项和引擎里那几段运行诊断全折在下面。
             大模型是黄字不是红字：出片那条路不用它。 -->
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
            <span v-else-if="!engineOnline" class="pill pill--danger">引擎连不上</span>
            <span class="spacer" />
            <!-- 这一页不会自己更新（配置文件能在外面改），这是少数该有刷新的地方 -->
            <button class="btn btn--ghost btn--sm" type="button" :disabled="loading" @click="load">
              <AppIcon name="refresh" :size="14" :class="{ spin: loading }" />
              重新体检
            </button>
          </div>
          <div v-if="overview?.doctor" class="stack stack--sm">
            <div
              v-for="c in failedChecks"
              :key="c.name"
              class="check"
              :class="`check--${c.level}`"
            >
              <AppIcon name="warn" :size="14" />
              <span class="check__name nowrap">{{ c.name }}</span>
              <span class="check__detail">{{ c.detail }}</span>
              <span v-if="c.fix" class="check__fix tiny dim">{{ c.fix }}</span>
            </div>
            <details class="fold">
              <summary class="fold__t">
                {{ failedChecks.length ? `另外 ${okChecks.length} 项都过了` : `全部 ${okChecks.length} 项都过了` }}
                <template v-if="embedded"> · 运行诊断</template>
              </summary>
              <div class="stack stack--sm">
                <div v-for="c in okChecks" :key="c.name" class="check check--ok">
                  <AppIcon name="check" :size="14" />
                  <span class="check__name nowrap">{{ c.name }}</span>
                  <span class="check__detail">{{ c.detail }}</span>
                </div>
                <div class="check check--ok">
                  <AppIcon name="check" :size="14" />
                  <span class="check__name nowrap">步数</span>
                  <span class="check__detail mono">
                    出片 {{ effective?.finalSteps ?? '—' }}
                    <span
                      v-if="effective?.turbo"
                      class="pill pill--ok tiny"
                      :title="`挂着 Turbo LoRA，出视频按 6 步走（档位表推的是 ${effective?.tableSteps} 步）`"
                    >Turbo</span>
                    <span v-else-if="effective?.stepsPinned" class="pill pill--neutral tiny">配置里写死</span>
                    · 首帧 {{ effective?.frameSteps ?? '—' }}
                  </span>
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
              </div>
            </details>
          </div>
        </section>

        <!-- 界面。**这一节是拖拽换边的替代品。**
             项目库那条栏原来靠拖栏头换边（约 110 行 + 一个只为
             setPointerCapture 存在的守卫函数），而靠哪边一辈子设一次，
             是「改的时候才要的」。主题不在这儿：顶栏右上那个图标就是它。 -->
        <section id="sec-look" class="sec">
          <div class="sec__head">
            <h2 class="sec__t">界面</h2>
          </div>
          <label class="field">
            <span class="field__label">项目库靠哪边</span>
            <select v-model="ui.railSide" class="select">
              <option value="right">右边</option>
              <option value="left">左边</option>
            </select>
          </label>
        </section>

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

            <p class="tiny dim mono">配置文件：{{ node.configFile }}</p>
          </div>
        </section>

        <!-- 引擎连不上时下面这几节一个都别摆：它们全是"这台引擎怎么配"，
             而那时候读不到任何值，摆出来是一排空框。 -->
        <template v-if="engineOnline">

          <!-- 配音 -->
          <section id="sec-tts" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">配音</h2>
              <span v-if="ttsDirty" class="pill pill--warn tiny">未存</span>
              <span class="spacer" />
              <div class="sec__acts">
                <!-- **只存配音这两项。** 以前这个按钮连大模型那几项一起提交，
                     而大模型那一节自己没有按钮——见 saveConn 上面那段。 -->
                <button
                  class="btn btn--primary btn--sm"
                  type="button"
                  :disabled="isBusy('conn')"
                  title="保存配音后端和地址，保存完重新体检"
                  @click="saveTts"
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
              <!-- 选「内置」时不摆一个灰掉的空框，选 HTTP 才出现 -->
              <label v-if="conn.tts_backend === 'http'" class="field">
                <span class="field__label">服务地址</span>
                <input v-model="conn.tts_base_url" class="input mono" placeholder="必填" />
              </label>
              <!-- **这里原来还有「时长容差」和「变速安全区」两个输入框，
                   2026-09-13 删了。** 它们和 [tts].engine 一样：有校验、
                   有持久化、能改，但引擎里**没有任何一个阶段读过**，而
                   提示语写的是"超出靠尾帧冻结或变速吸收"——那个行为不存在。
                   台词装不下实际走的是按实测语速重切一次再合成
                   （stages/audio.cpp）。一个能改却什么都不做的旋钮比没有
                   更糟：人调完以为生效了。 -->
            </div>
          </section>

          <!-- 模型目录和下载。**只管往哪儿下，不管下什么**——挑模型在项目页。 -->
          <section id="sec-models" class="sec">
            <div class="sec__head">
              <h2 class="sec__t" title="整台机器共用；挑哪个模型在项目页">模型目录和下载</h2>
              <span class="spacer" />
              <button
                class="btn btn--primary btn--sm"
                type="button"
                :disabled="!modelsDirty || isBusy('modelsDir')"
                @click="saveModelsDir"
              >
                {{ isBusy('modelsDir') ? '存着…' : '保存' }}
              </button>
            </div>
            <div class="grid grid--2">
              <label class="field">
                <span class="field__label">模型目录</span>
                <input v-model="modelsDir" class="input mono" placeholder="留空用默认位置" />
              </label>
              <label class="field">
                <span class="field__label">下载源</span>
                <select v-model="modelsSource" class="select">
                  <option v-for="o in modelsSources" :key="o.id" :value="o.id">
                    {{ o.label }}<template v-if="o.note"> · {{ o.note }}</template>
                  </option>
                </select>
              </label>
            </div>
            <!-- **没有下载器才提示怎么装。** 原来这两行安装命令是无条件印在
                 页面上的，而装了 aria2 的机器上它一年也用不着。 -->
            <p v-if="!modelsTool" class="tiny bad">
              这台机器上没找到下载器（aria2c 或 curl），下不了模型。
              Debian/Ubuntu 装：apt-get install -y aria2；Windows：winget install aria2.aria2
            </p>
          </section>

          <!-- 装配与闸门。**一个按钮一起存，就是一节。** 原来是两节，
               「保存」只在闸门那节、提示写着"装配和闸门一起保存"。 -->
          <section id="sec-assembly" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">装配与闸门</h2>
              <span class="spacer" />
              <div class="sec__acts">
                <button
                  class="btn btn--primary btn--sm"
                  type="button"
                  :disabled="isBusy('params')"
                  @click="saveParams"
                >
                  {{ isBusy('params') ? '保存中…' : '保存' }}
                </button>
              </div>
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
              <!-- 「转场（秒）」删了：assemble.cpp 写着"xfade / acrossfade /
                   fade= 都没有，转场从来没被渲染过"。配置键留着，界面上不摆
                   一个什么都不做的旋钮。 -->
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
            <div class="stack">
              <label class="switch">
                <input v-model="params.enabled" type="checkbox" />
                <span>闸门开启</span>
              </label>
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
              </div>
              <!-- 原来这里写「重试超限降级为静帧加运镜」。**没有那回事**：
                   引擎的 fallback 只是留着最后那一版视频，全代码库一处
                   zoompan 都没有（2026-09-13 查过）。 -->
              <label
                class="switch"
                title="关掉的话，重试超限的镜头会标成「未过闸门」，整集停在那儿等人"
              >
                <input v-model="params.fallback_on_exhausted" type="checkbox" />
                <span>重试超限就留着最后那一版（没过闸门也照用）</span>
              </label>
            </div>
          </section>

        </template>

      </div>
    </div>
  </div>
</template>

<style scoped>
.fold__t {
  color: var(--text-3);
  font-size: var(--fs-xs);
  cursor: pointer;
}
.fold[open] > .fold__t {
  margin-bottom: 6px;
}
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
/* ⚠️ **这一档的类名是 `fail`，不是 `error`。**
   引擎发的只有 ok / warn / fail 三个值（doctor.hpp::to_string），而这儿
   原来写的是 `.check--error`——拼出来的 `check--fail` 没有任何规则命中，
   于是**最严重的那几项反而最不显眼**：warn 有黄底黄图标，fail 落回
   .check 的灰底加一个灰图标，比警告还淡。而这一节就是拿来看"卡在哪"的，
   「还不能跑」那颗红丸子指的正是这几条。
   ProjectView 的进度条栽过同一种：拼出来的 modifier 一个都没定义。 */
.check--fail {
  background: var(--danger-soft);
}
.check--fail :deep(svg) {
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
</style>
