<script setup>
/**
 * 第三步：角色。
 *
 * 角色外观只存在这里，分镜表里只有 id。所以这一页改一个字，
 * 全剧几十个镜头的提示词都跟着变——引擎会把已渲染的镜头退回重跑，
 * 界面必须把这件事说在前面，别让人改完才发现成片全没了。
 */
import { computed, nextTick, onActivated, onDeactivated, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { useAction } from '@/composables/useAction'
import { runAsyncJob } from '@/composables/useAsyncJob'
import { useRefStream } from '@/composables/useRefStream'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const props = defineProps({
  /** 故事里的人物关系。只为抽屉里那几行，墙上不显示。 */
  relations: { type: Array, default: () => [] },
})

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

/**
 * 每一格画到百分之几。
 *
 * 出图现在是异步的：接口当场回"开始了"，采样进度从 WebSocket 一步一步
 * 推回来（见 useAsyncJob）。**有了这个数，等待才不是一片空白**——一张
 * 几十秒，而头十几秒还在把模型读进显存，那段时间一步都不会推。
 */
/**
 * 进度、采样中途那张小图、这一格在不在跑——**都从那条固定频道来**。
 *
 * 用户 2026-09-12 两件事：「画图方式也要实时返回步数图」，和「我刷新了
 * 这个页面，正在生成的图就不会实时更新」。后者的根子是进度原来只走"这一次
 * 点击"那条随机 id 的 stream，刷新就断了；现在走 useRefStream 那条固定的。
 *
 * key 是 `char_id_slot`——**和引擎那边的 target 逐字一样**，不然对不上。
 */
const { pct: genPct, preview, live, finished, touch } = useRefStream()

/** 引擎那边怎么叫这一格。改这里就得改 ref_gen.cpp 里拼 stem 那一行。 */
const targetOf = (charId, slot) => `${charId}_${slot}`


/** 抽屉里改的是哪一个。 */
const openChar = computed(
  () => characters.value.find((c) => c.char_id === openId.value) ?? null,
)

/**
 * 这一格在不在画。三张里任意一张在画，整张牌子就算在跑。
 *
 * **`live` 要算进去**：刷新过页面之后 `isBusy` 一定是假的（那是本地点击
 * 留下的状态，刷新就没了），而活还在跑——那时候唯一的依据就是频道上
 * 有没有消息。
 */
function cellBusy(charId) {
  if (isBusy('genall:' + charId)) return true
  return SLOTS.some(
    (s) => isBusy('gen:' + charId + s.key) || live[targetOf(charId, s.key)],
  )
}

/** 这一格画到百分之几。没数就回 null，界面画一条来回跑的条。 */
function cellPct(charId) {
  for (const s of SLOTS) {
    const v = genPct[targetOf(charId, s.key)]
    if (v) return v
  }
  return null
}

const assets = ref(null)
const loading = ref(false)
const openId = ref('')
/**
 * 抽屉开关时的焦点。
 *
 * 这个抽屉是 `position: fixed; inset: 0` 的全屏遮罩，和那三个弹窗一个性质：
 * 用键盘按开它之后，焦点还留在**遮罩后面**那颗按钮上，按 Tab 是在看不见的
 * 页面里走；关掉之后焦点落到 `<body>`，下一次 Tab 得从整页开头重走。
 *
 * 开：焦点放到面板本身（tabindex="-1"），不猜第一个控件。盯的是面板出现
 * 那一刻——模板 ref 是响应式的，元素挂上来就聚焦。
 * 关：还给把它叫起来的那颗按钮。**只在从"没开"到"开"时记一次**，换一条
 * （不关抽屉直接点另一个）时不重记，否则记下的会是抽屉里面的元素。
 */
const panel = ref(null)
watch(panel, (el) => el?.focus())
let opener = null
watch(openId, (now, before) => {
  if (now && !before) {
    opener = document.activeElement
    return
  }
  if (!now) {
    const back = opener
    opener = null
    back?.focus?.()
  }
})

const edits = ref({}) // char_id -> 编辑中的副本
const voices = ref([])
const voicesError = ref('')
const voicesLoading = ref(false)
// 「三张一起画」画到哪一张了。三张各要几十秒，不报的话按钮上就是一句
// 不动的「画着…」，用户分不清是在画还是卡住了。
const genStep = ref(null)

const SLOTS = [
  { key: 'front', label: '正面' },
  { key: 'three_quarter', label: '四分之三侧面' },
  { key: 'back', label: '背面' },
]

const FIELDS = [
  { key: 'identity', label: '身份', hint: '性别、年龄段、气质。这一段权重最高。', rows: 2 },
  { key: 'body', label: '体型', hint: '身高感、体态。可留空。', rows: 2 },
  { key: 'face', label: '五官发型', hint: '脸型、发型、发色、瞳色。认脸靠它。', rows: 3 },
  { key: 'attire', label: '默认服装', hint: '换装状态另设，这里写常态。', rows: 2 },
  { key: 'style', label: '专属画风', hint: '只加在这个角色身上的修饰。可留空。', rows: 2 },
]

/**
 * 筛这一格。**和场景那格同一套**（见 AssetLocations）——人多起来时，
 * 要找的多半是"哪几个还没画全"，而不是某一个具体的人。
 */
const q = ref('')

/** 这张牌要不要缩成一半高：三张全空、而且这会儿也没在画。 */
function flat(c) {
  return !SLOTS.some((s) => c['ref_' + s.key]) && !cellBusy(c.char_id)
}

/** 跟这个人有关的关系。定妆的依据，挨着它服务的那个人。 */
function relsOf(c) {
  return props.relations.filter((r) => r.a === c.name || r.b === c.name)
}

/** 手里这份角色是哪部剧读回来的。null = 手上没有。 */
let loadedFor = null
/** 上一趟为什么没读回来。空 = 没出事。提示条会消失，这一行不会。 */
const loadError = ref('')

const all = computed(() => assets.value?.characters ?? [])
const characters = computed(() =>
  all.value.filter((c) => {
    const k = q.value.trim().toLowerCase()
    if (k && !String(c.name || '').toLowerCase().includes(k)) return false
    return true
  }),
)
const refsUsed = computed(() => assets.value?.reference_images_used)

async function load() {
  if (!session.projectPath) {
    loading.value = false // 理由同镜头墙那处：被顶掉的那趟不会清它
    return
  }
  // 换剧时两趟会叠在一起，慢的那趟后落地就把上一部的角色摆在这一部下面
  const want = session.projectPath
  loading.value = true
  try {
    const before = edits.value
    const got = await api.assets(session.projectPath)
    if (want !== session.projectPath) return
    assets.value = got
    loadedFor = want
    loadError.value = ''
    // ⚠️ **改了还没存的那一条不能被盖掉。**
    //
    // 这个 load 不只在换项目时跑：`watch(finished, load)` 让它在**每画完
    // 一张参考图**的时候也跑一遍。而一键出图是一张接一张、每张几十秒——
    // 抽屉里正在改外观描述的人，每隔几十秒手底下的字就被服务端那份原样
    // 盖回去一次，一声不吭。单独给某个角色出一张图也一样。
    //
    // 照故事页那条既有规矩办（`setStory` 只刷新不在 dirtySnapshot 里的章）：
    // **只刷新没改过的那几条**。先把新的资产库装上，`changed()` 比的就是
    // 「手里这份」和「刚拿回来这份」，正好是要的判据。
    //
    // 三视图的缩略图不受影响：它们绑的是 `c['ref_' + slot]`，从 assets
    // 来，不从 edits 来——所以留着草稿不会把刚画完的图冻在旧的上。
    edits.value = Object.fromEntries(
      (assets.value.characters ?? []).map((c) => [
        c.char_id,
        before[c.char_id] && changed(c.char_id) ? before[c.char_id] : { ...c },
      ]),
    )
  } catch (err) {
    if (want !== session.projectPath) return
    // **墙上这份要是上一部剧的，就得撤下来。**
    //
    // 这儿原来只弹一句错。而换剧这条路是先清 edits 再 load 的，load 砸了
    // 的话 `assets` 一个字没动——新这一部的页面上摆着**上一部的角色墙**，
    // 点开一张，抽屉里是空的（edits 刚清过），而右上角那几个按钮按的是
    // 现在这一部。八秒之后提示条自己消失，剩下一面对不上号的墙。
    //
    // 只在手里这份属于别的项目时撤。同一部剧自己刷新失败（画完一张图那条
    // 订阅每几十秒就来一趟）不撤——那时候屏幕上的就是这一部自己的东西，
    // 为一次网络抖动把整面墙清掉更糟。
    if (loadedFor !== session.projectPath) {
      assets.value = null
      edits.value = {}
      openId.value = ''
      loadedFor = null
      loadError.value = err.message
    }
    ui.error(err.message)
  } finally {
    if (want === session.projectPath) loading.value = false
  }
}

function onEsc(e) {
  // 抽屉盖着半个屏幕，而鼠标多半正停在里面——Esc 是唯一不用先瞄准的出口。
  if (e.key === 'Escape' && openId.value) openId.value = ''
}

/**
 * 刷新之前拦一下：抽屉里改了还没存的那几条。
 *
 * 这一页没有自动保存（外观描述改完要自己点保存），而 `edits` 里可以同时
 * 挂着好几条改动——按 Esc 收抽屉是**不还原**的（有意的：那一下只是收起来，
 * 不是放弃）。收起来之后刷新一下，改了半天的外观描述一声不吭地没了。
 *
 * 收抽屉那条路早就会问一句（见 toggle），唯独刷新和关标签页不会。
 */
function beforeUnload(e) {
  if (!Object.keys(edits.value).some((id) => changed(id))) return
  e.preventDefault()
  e.returnValue = ''
}
onMounted(() => {
  document.addEventListener('keydown', onEsc)
  window.addEventListener('beforeunload', beforeUnload)
  loadPresets()
})
onUnmounted(() => {
  document.removeEventListener('keydown', onEsc)
  window.removeEventListener('beforeunload', beforeUnload)
  // 走开就别响了
  player?.pause()
  player = null
})

/**
 * **切到别的格子就把 Esc 让出去。**
 *
 * 设定那三格被 AssetsView 的 `<KeepAlive>` 冻着——切走是**停用**不是卸载，
 * `onUnmounted` 不跑，这个 keydown 照样挂在 document 上。而 `onEsc` 只看
 * 自己的 `openId` 非空，切格子并不会关抽屉：在场景格按一下 Esc，隔壁角色
 * 格那个看不见的抽屉跟着一起关了，回去才发现它自己合上了。
 *
 * 镜头页早有这道（见 EpShots 里 onActivated 那段）。重复 add 同一个函数
 * 引用是安全的（DOM 去重），首次挂载时两个钩子都跑一遍没关系。
 */
onActivated(() => document.addEventListener('keydown', onEsc))
onDeactivated(() => document.removeEventListener('keydown', onEsc))

/**
 * **换项目要先把手里那份编辑清掉。**
 *
 * `load()` 里那段「只刷新没改过的」是为 `watch(finished, load)` 写的——
 * 一键出图一张接一张，每张几十秒就重拉一次，抽屉里正在改字的人不能被
 * 服务端那份一遍遍盖回去。
 *
 * 但**换项目走的是同一个 load**，而 edits 是按 id 索引的：两部剧里出现
 * 同一个 id 不是稀奇事（id 是照名字生成的，续集、复制出来的项目、同名
 * 角色都会撞），撞上的那一条会被当成"这条人家改过、别刷新"，于是上一部
 * 的外观描述留在这一部的抽屉里，一存就写进去了。
 *
 * 所以换项目这条路先清空再读：那时候"手里改了还没存的"本来就属于上一部，
 * 冲不冲得掉是另一回事（这一页没有自动保存，走开就是丢），但绝不能跟着
 * 到下一部去。抽屉也一并收起来——它开着的是上一部那一条。
 */
watch(
  () => session.projectPath,
  () => {
    edits.value = {}
    openId.value = ''
    load()
  },
  { immediate: true },
)
// 频道上说哪一张画完了就重拉——**刷新过页面的人只剩这条路**：
// 发起那次请求的 promise 早随着旧页面一起没了。
watch(finished, load)

/**
 * 问服务端有哪些参考音色。
 *
 * **现在两条配音后端都没有服务端清单**，这一问回的是一句说明：
 * 进程内配音的音色是用户自己给的参考音频，外部服务的音色由那个服务自己管。
 * 接口留着是因为它承担了"告诉用户音色怎么配"这件事——那句话比一个空
 * 下拉框有用。「正在问」的状态也留着：接口本身还是异步的。
 */
async function loadVoices() {
  voicesError.value = ''
  voicesLoading.value = true
  try {
    const data = await api.voices(session.projectPath)
    voices.value = data.voices ?? []
    if (data.error) voicesError.value = data.error
  } catch (err) {
    voicesError.value = err.message
  } finally {
    voicesLoading.value = false
  }
}

async function toggle(charId) {
  // 改了外观直接收起来，改动就没了。先问一句。
  if (openId.value && changed(openId.value)) {
    if (!confirm('这个角色有改动还没保存，收起就没了。确定？')) return
    // 放弃的那一份要还原，否则「未保存」的红标会一直挂在列表上
    const was = characters.value.find((c) => c.char_id === openId.value)
    if (was) edits.value[openId.value] = { ...was }
  }
  openId.value = openId.value === charId ? '' : charId
  if (!openId.value) return
  if (!voices.value.length && !voicesError.value) loadVoices()
  // 展开的那一块很高。点的是列表靠下的角色时，内容全在屏幕外面，
  // 看上去就像「点了没反应」。等一帧渲染完再把它拉回视野里。
  await nextTick()
  document
    .querySelector(`[data-char="${charId}"]`)
    ?.scrollIntoView({ behavior: 'smooth', block: 'start' })
}

function changed(charId) {
  const now = edits.value[charId]
  const was = characters.value.find((c) => c.char_id === charId)
  if (!now || !was) return false
  return [...FIELDS.map((f) => f.key), 'name', 'voice_id', 'lora_trigger'].some(
    (k) => (now[k] ?? '') !== (was[k] ?? ''),
  )
}

/**
 * 照故事给人定妆。
 *
 * **这一步不创作新的人**——人是故事里定的，这里只是把故事里那份名单翻成
 * "长什么样"：身份、体型、五官发型、默认服装。有故事就从故事里读，没有
 * 才回落到剧本（引擎那边的 source=auto）。
 *
 * 角色是全剧共用的一批，所以不指定集号。默认只补还没定过妆的人：老角色
 * 的设定和参考图都留着。第五集冒出一个新角色时，不该把前四集主角的脸
 * 重新想一遍。
 */

async function save(charId) {
  const draft = edits.value[charId]
  const patch = {}
  for (const key of [...FIELDS.map((f) => f.key), 'name', 'voice_id', 'lora_trigger']) {
    if (draft[key] !== undefined && draft[key] !== null) patch[key] = draft[key]
  }
  const result = await run(
    () => api.saveCharacter({ project: session.projectPath, char_id: charId, patch }),
    { key: 'save:' + charId, refresh: true },
  )
  if (!result) return
  ui.ok(
    result.reset_shots
      ? `已保存，${result.reset_shots} 个镜头退回重跑`
      : '已保存',
  )
  await load()
}

async function upload(charId, slot, event) {
  const file = event.target.files?.[0]
  if (!file) return
  const form = new FormData()
  form.append('project', session.projectPath)
  form.append('char_id', charId)
  form.append('slot', slot)
  form.append('file', file)
  const result = await run(() => api.uploadReference(form), { key: 'up:' + charId + slot })
  event.target.value = ''
  if (!result) return
  // **没退就别提退。** 这一页正常是在分镜之前用的，`reset_shots` 是 0 才是
  // 常态——不判的话它写「0 个镜头退回重跑」，报一件没发生的事。同一份判断
  // 在 saveCharacter、改画风、关联场景那几处都有，这儿和空景图那儿漏了。
  ui.ok(
    `参考图已存（${result.size_kb} KB）` +
      (result.reset_shots ? `，${result.reset_shots} 个镜头退回重跑` : ''),
  )
  // **招呼一声就够。** 传图和撤图都不走 `ref_done`（那条只有出图才发），
  // 而这一格和设定页 tab 上那个「缺 N」都订着 finished——touch 一下两边
  // 一起重拉。自己再 `await load()` 的话，同一个 /api/assets 打两次。
  // `bible()` 那儿用的就是这个写法。touch 的注释里说的「资产库变了但不是
  // 画完一张」，就是这两下。
  touch()
}

// ---- 制作音色 ----
//
// **为什么是"摇"不是"描述"。** 我们这条运行时（llama.cpp 的 mtmd）只实现
// 了 Qwen3-TTS 的 Base 模式，也就是参考音频克隆；自带说话人的 CustomVoice
// 和用文字描述造音色的 VoiceDesign 都不在里面。而不给参考音频时，说话人是
// 和内容一起被采样出来的——**种子换一个就是换一个人**。
//
// 所以流程是：摇一个 → 试听 → 不满意再摇 → 满意了起个名存下来。存下来的
// 是一段音频，从此这个角色被克隆锁死，再也不会变。
const presets = ref([])
const take = ref(null)        // { rel, seed, seconds, hz }
const takeName = ref('')

async function loadPresets() {
  try {
    presets.value = (await api.voicePresets()).presets ?? []
  } catch {
    presets.value = []
  }
}

/**
 * 试听用的播放器。**整页只留一个。**
 *
 * 原来两处试听都是 `new Audio(url).play()`——每一下都新建一个谁也抓不住的
 * 播放器。连点两下「试听」就是两段声音叠着放，而摇音色这件事的流程本来就
 * 是「摇一个 → 试听 → 不满意再摇」，连点是常态；试听完走开也停不下来，
 * 组件卸了它还在响。留一个句柄，放下一段之前先把上一段按停。
 */
let player = null

function playAudio(rel, url) {
  player?.pause()
  player = new Audio(url)
  player.play().catch(() => {
    ui.info('浏览器挡住了自动播放，音频存在 ' + rel)
  })
}

/** 播一段刚摇出来的。加时间戳绕开缓存——落点是固定的那个 .take.wav。 */
function playRel(rel) {
  playAudio(rel, mediaUrl(session.projectPath, rel) + '&_=' + Date.now())
}

/** 摇一个。不给种子就让引擎随机，回包里带着它——喜欢这一摇才存得下来。 */
async function rollVoice(seed = null) {
  const result = await run(
    () => api.voiceTake({ project: session.projectPath, ...(seed === null ? {} : { seed }) }),
    { key: 'take' },
  )
  if (!result) return
  take.value = result
  playRel(result.rel)
}

/**
 * 存成音色。
 *
 * **存的就是刚才听的那一段**：引擎直接把 `.take_<seed>.wav` 拷成音色文件。
 *
 * 原来是"按同一个种子重出一段更长的"（参考音频越长克隆越稳）。那是错的：
 * 音色是 (种子, 文本) 的函数，换了文本就换了人——实测 1789 号从 112 Hz
 * 变成 205 Hz、3313 号从 137 变成 224，存进去的根本不是试听里那个声音。
 */
async function saveTake(charId) {
  if (!take.value) return
  const result = await run(
    () =>
      api.voiceSave({
        project: session.projectPath,
        seed: take.value.seed,
        name: takeName.value,
        char_id: charId,
      }),
    { key: 'savetake' },
  )
  if (!result) return
  if (edits[charId]) edits[charId].voice_id = result.saved
  ui.ok(`音色「${result.name}」存好了，已挂到这个角色上。去镜头墙点「配音」让它生效`)
  take.value = null
  takeName.value = ''
  await loadVoices()
  await load()
}

/**
 * 传一段参考音色。
 *
 * **这就是这一版的"选音色"。** 进程内配音没有服务端的音色清单——音色来自
 * 一段人声片段，模型照着它的音色念（tts_backends.cpp 里 `speaker_ref`）。
 * 以前这一栏只有一个空输入框，要用户手打路径，打错的表现只是配音失败。
 *
 * 传完**不会自动重配音**：音色和外观不一样，它不影响画面，所以和改名字
 * 一个待遇（editing_assets.cpp 里 kAppearance 不含 voice_id）。要让它生效，
 * 去镜头墙对那几镜点「配音」。
 */
async function uploadVoice(charId, event) {
  const file = event.target.files?.[0]
  if (!file) return
  const form = new FormData()
  form.append('project', session.projectPath)
  form.append('char_id', charId)
  form.append('file', file)
  const result = await run(() => api.uploadCharacterVoice(form), {
    key: 'voice:' + charId,
  })
  event.target.value = ''
  if (!result) return
  if (edits[charId]) edits[charId].voice_id = result.saved
  ui.ok(`参考音色已存（${result.size_kb} KB）。去镜头墙点「配音」让它生效`)
  await loadVoices()
  await load()
}

async function clearVoice(charId) {
  const result = await run(
    () => api.clearCharacterVoice({ project: session.projectPath, char_id: charId }),
    { key: 'voice:' + charId },
  )
  if (!result) return
  if (edits[charId]) edits[charId].voice_id = ''
  ui.ok('参考音色已撤，配音会退回自动挑')
  await loadVoices()
  await load()
}

/**
 * 试听：拿这个角色当前的音色念一句。
 *
 * **念的是这个角色自己的台词**（找不到就用一句通用的）——听"某某某"念
 * 一段和剧本无关的话，判断不出这个音色配不配得上这个人。
 */
async function tryVoice(charId) {
  const voice = edits[charId]?.voice_id || ''
  const c = characters.value.find((x) => x.char_id === charId)
  const text = `你好，我是${c?.name || charId}。这是我说话的样子。`
  const result = await run(
    () => api.say({ project: session.projectPath, text, voice }),
    { key: 'say:' + charId },
  )
  if (!result) return
  // **estimate 后端出来的是静音。** 不说的话，用户对着一段没声音的音频
  // 会以为是自己音箱坏了——引擎照实回了 backend，这里照实说。
  if (result.backend === 'estimate') {
    ui.warn('现在的配音后端只算时长不出声（estimate），听不到东西是正常的')
  }
  // 字段是 `rel` 不是 audio_path，见 tts_api.cpp 的返回体。
  playAudio(result.rel, mediaUrl(session.projectPath, result.rel) + '&_=' + Date.now())
}

/**
 * 照着上面那段"拼出来的提示词"现画一张。
 *
 * **这是这一页上唯一真正在用 AI 的地方**（还有场景那张空景图）：人是谁、
 * 要什么、什么关系，都在故事里定完了；这一页只负责把那些人翻成可画的
 * 描述，再照着描述画出来。
 *
 * 一张几十秒，头一张还要先把出图模型读进显存。所以每个位置各自有自己的
 * 忙碌状态，画着的那张只停自己那一格，另外两格照样能点。
 */
async function genRef(charId, slot) {
  const result = await run(
    () =>
      runAsyncJob(
        (extra) =>
          api.generateReference({
            project: session.projectPath,
            char_id: charId,
            slot,
            ...extra,
          }),
        // 进度和预览都从那条固定频道来（useRefStream），这儿不用再接一遍
        // ——接两遍等于同一张几十 KB 的小图收两次。
        { prefix: 'ref' },
      ),
    { key: 'gen:' + charId + slot },
  )
  if (!result) return
  // 出图那一趟同样会退镜头（引擎注释：「和上传那条一样**无条件重跑**：
  // 参考图直接决定画面长什么样」），回包里带着数，这儿原来只报秒数。
  ui.ok(
    `${SLOTS.find((s) => s.key === slot)?.label ?? slot}画好了（${Math.round(result.seconds)} 秒）` +
      (result.reset_shots ? `，${result.reset_shots} 个镜头退回重跑` : ''),
  )
  await load()
}

/** 三张一起画。**一张一张来**：显存只够一张，并发只会排队，还看不出进度。 */
async function genAllRefs(charId) {
  // **已经有的会被顶掉，要先问。**
  //
  // 设定页那个「全部重画」为同一件事准备了确认框，理由写在它旁边：
  // 「已经画好的那些多半是挑过的——有的还是手传上去的真人照片。一键把它们
  // 全顶掉，等于一次点击毁掉半小时的挑选，而这种事没有撤销。」这颗按钮对
  // **这一个角色**做的是一模一样的事（三个槽位挨个重画，手传的照片一样被
  // 顶掉），却一句不问；而它的名字「三张一起画」听着像是"把缺的补齐"。
  //
  // 只在真有东西会被顶掉时问，并且报实数——没有的话直接画，别拿一个多余的
  // 确认框挡住正常那条路。单张那个「重画」不问：点的就是那一张，意思已经
  // 说明白了。
  const mine = characters.value.find((c) => c.char_id === charId)
  const have = SLOTS.filter((s) => mine?.['ref_' + s.key]).length
  if (
    have &&
    !confirm(
      `会把这个角色已有的 ${have} 张参考图重画一遍，手传上去的也会被顶掉；` +
        // 同设定页那颗「全部重画」：出图会让引擎把全项目已渲染的镜头退回
        // 待跑，这一项比"重画三张"本身重。
        `已经渲染好的镜头也会退回重跑。确定？`,
    )
  ) {
    return
  }
  // 三张要跑一分多钟。中途换了剧的话，后面那两张会拿这一部的 char_id 去
  // 新那一部出图（id 在那边不存在，404）。活儿是替这一部排的，钉住它。
  const project = session.projectPath
  for (const s of SLOTS) {
    genStep.value = { charId, label: s.label }
    const ok = await run(
      () =>
        runAsyncJob(
          (extra) =>
            api.generateReference({
              project,
              char_id: charId,
              slot: s.key,
              ...extra,
            }),
          {
            prefix: 'ref',
            // 「三张一起画」那个按钮上要写第几张，所以这条还留着。
            onProgress: (cur, total) => {
              genStep.value = {
                charId,
                label: s.label,
                pct: total > 0 ? Math.round((cur / total) * 100) : 0,
              }
            },
          },
        ),
      { key: 'genall:' + charId },
    )
    // 中间某一张失败就停：后面两张多半也会栽在同一件事上（模型没配、
    // 显存不够），接着画只是让用户多等两分钟再看到同一句报错。
    if (!ok) break
  }
  genStep.value = null
  await load()
}

async function clearRef(charId, slot) {
  // **这一下会把全项目已渲染的镜头退回待跑。**
  //
  // 引擎撤完就调 reset_all_shots（参考图直接决定画面长什么样），而这颗按钮
  // 在界面上只是抽屉里一个小小的「撤掉」。这个库里比它轻的动作都问一句
  // ——定妆覆盖、一键全部重画、三张一起画、删章删集删项目，全都问。
  //
  // 顺带说清另一件事：文件本身留在盘上（引擎那儿写着"用户可能只是想先试试
  // 没有参考图的效果"），但界面上接不回来——要用回那张图得重新传一遍。
  if (!confirm('撤掉这张参考图？已经渲染好的镜头会退回重跑；图片文件留在盘上，但界面上接不回来，要用回它得重新传一遍。')) return
  const result = await run(
    () => api.clearReference({ project: session.projectPath, char_id: charId, slot }),
    { key: 'clr:' + charId + slot },
  )
  if (result) {
    // 回包里有 `cleared` 和 `reset_shots`，这儿原来一个都没读。
    //
    // **撤一张参考图会把已渲染的镜头退回待跑**（引擎那边撤完就调
    // reset_all_shots），而提示里一个字没有——隔壁「上传」和「保存」都报
    // 这件事，独独撤图不报。人撤掉一张图，整屏的「成片完成」在下一次重拉
    // 之后变成「未开工」，而他刚看到的提示只说了「已撤掉」。
    //
    // `cleared` 为假是"本来就没有这张图"（引擎提前返回、一镜没退）。
    ui.ok(
      result.cleared === false
        ? '这个位置本来就没有参考图'
        : '已撤掉这张参考图' +
            (result.reset_shots ? `，${result.reset_shots} 个镜头退回重跑` : ''),
    )
    touch() // 同 upload：撤图不发 ref_done，这一格和「缺 N」都靠它重拉
  }
}
</script>

<template>
  <div class="chars">
    <!-- 人是故事里定的，这一页只给他们定妆。工具行：刷新、重定、照故事定。 -->
    <!-- 只剩一个搜索框。刷新（页面订着 refs 频道）、只看缺图（一键出图
         本来就把缺的全画了）、定妆和覆盖的勾（页级动作，在 tab 那一行）——
         2026-09-14 都从这儿撤了。
         ⚠️ **搜索框不要加"多了才显示"的门槛。** 加过一次（>8），两个角色的
         项目上一个搜索框都没有，用户看到的就是"一点变化都没有"。 -->
    <div class="toolbar">
      <span class="spacer" />
      <input v-model="q" class="input find" placeholder="找人" />
    </div>

    <!-- **不要在这儿套一个光秃秃的 <template>**：Vue 只把带
         v-if/v-for/v-slot 的 template 当片段，没有指令的会当成真的
         HTML template 元素渲染出去——浏览器默认 display:none，
         内容全在 DOM 里、一个字不报错，就是看不见。栽过一次。 -->
      <!-- **一句结论，理由折起来。** 引擎给的这段是 112 个字，横在工具行
           和角色墙中间；而它每次说的都是同一件事，读完第一遍之后就只是一堵
           墙。摘要说结论，展开才是"为什么、怎么办"。 -->
      <details v-if="refsUsed === false" class="fold fold--warn">
        <summary class="fold__t warn-text">参考图不会照着画</summary>
        <p class="tiny">{{ assets?.reference_hint }}</p>
      </details>

      <!-- **读不出来就说读不出来。** 下面那两条空状态说的是「库里没有」，
           而读砸了的时候库里有没有根本不知道——提示条八秒就没了，剩一句
           「还没有角色」挂在那儿，看着像东西丢了。 -->
      <EmptyState
        v-if="!loading && loadError"
        icon="warn"
        tone="warn"
        title="读不到这部剧的设定"
        :hint="loadError"
      />

      <EmptyState
        v-else-if="!loading && !characters.length && all.length"
        icon="search"
        title="没有对得上的"
        hint="换个词试试"
      />

      <!-- **故事已经写了的时候别再说「先写故事」。** 那句话把人支去一个
           他刚来的地方，而真正差的一步就在这一页右上角。两种状态两句话：
           判据用侧栏那个对勾同一份（done.story = 有章节）。 -->
      <EmptyState
        v-else-if="!loading && !characters.length && !session.done.story"
        icon="user"
        title="还没有角色"
        hint="先写故事，再点右上角「照故事定妆」"
      >
        <RouterLink to="/story" class="btn btn--sm">去写故事</RouterLink>
      </EmptyState>

      <EmptyState
        v-else-if="!loading && !characters.length"
        icon="user"
        title="还没有角色"
        hint="故事写好了，点右上角「照故事定妆」，让 AI 读一遍把人和地方定下来"
      />

      <!-- **一人一张牌，和「这一集」那面镜头墙一个样子。**
           用户 2026-09-12：「角色，场景的展示方式和这一集一样」。
           以前是一行一个人、头像只有三十几个像素——而这一页的产出就是图，
           把图做成一行里的小圆点，等于把要看的东西藏起来。 -->
      <div v-else class="wall">
        <article
          v-for="c in characters"
          :key="c.char_id"
          class="cell"
          :class="{
            'cell--live': cellBusy(c.char_id),
            'cell--open': openId === c.char_id,
            'cell--flat': flat(c),
          }"
          :data-char="c.char_id"
        >
          <!-- 三张各占三分之一，鼠标放上去那张摊开成全图。
               用户 2026-09-12：「角色3张图已1/3方式显示，鼠标放到上面展开
               成全图」。三张是正面、四分之三侧面、背面——挨着看才比得出
               是不是同一个人，而那正是参考图要回答的问题。 -->
          <div class="trio" @click="toggle(c.char_id)">
            <span
              v-for="s in SLOTS"
              :key="s.key"
              class="trio__one"
              :class="{ 'is-empty': !c['ref_' + s.key] }"
              :title="s.label"
            >
              <img
                v-if="c['ref_' + s.key]"
                :src="mediaUrl(session.projectPath, c['ref_' + s.key])"
                :alt="s.label"
                loading="lazy"
              />
              <AppIcon v-else name="image" :size="16" class="trio__blank" />

              <!-- 采样中途那张小图，盖在这一格上。低分辨率放大本来就是糊的，
                   随着步数推进内容逐渐成形；画完就没了（真图上来）。 -->
              <img
                v-if="preview[targetOf(c.char_id, s.key)]"
                class="trio__preview"
                :src="preview[targetOf(c.char_id, s.key)]"
                alt=""
              />
              <span v-if="genPct[targetOf(c.char_id, s.key)]" class="trio__pct numeric">
                {{ genPct[targetOf(c.char_id, s.key)] }}%
              </span>
              <span class="trio__label tiny">{{ s.label }}</span>
            </span>
          </div>

          <!-- 进度就是这一行的底色，和镜头墙一个规矩：铺成背景既不占地方，
               也比一条细线看得清。 -->
          <div class="cell__bottom">
            <span
              v-if="cellBusy(c.char_id)"
              class="cell__fill"
              :class="{ 'cell__fill--idle': cellPct(c.char_id) === null }"
              :style="cellPct(c.char_id) !== null ? { width: cellPct(c.char_id) + '%' } : null"
            />
            <button class="cell__name truncate" type="button" title="改这个人" @click="toggle(c.char_id)">
              {{ c.name }}
            </button>
            <span class="spacer" />
            <!-- 只剩「未保存」。0/3 和「音色/自动」删了：三个空格子本身就是
                 0/3，图能说的话不再用字说一遍；音色是改的时候才要的，在抽屉里。 -->
            <span v-if="changed(c.char_id)" class="pill pill--warn tiny">未保存</span>
          </div>

        </article>
      </div>

      <!-- 点开一个角色，从右边滑出来改。**和「这一集」那面镜头墙一个做法**
           （用户 2026-09-12：「展示方式和这一集一样」）：墙是用来挑的，
           抽屉是用来改的——把编辑器塞回牌子里会把那一格撑成一整行，
           一墙的牌子跟着重排，而人刚刚就是靠位置认出那张牌的。 -->
      <div v-if="openChar" class="drawer" @click.self="openId = ''">
        <aside ref="panel" class="drawer__panel" tabindex="-1">
          <header class="drawer__head">
            <b>{{ openChar.name }}</b>
            <span class="tiny dim mono">{{ openChar.char_id }}</span>
            <span v-if="changed(openChar.char_id)" class="pill pill--warn tiny">未保存</span>
            <span class="spacer" />
            <button class="iconbtn" type="button" title="收起（Esc）" @click="openId = ''">
              ✕
            </button>
          </header>

          <!-- 跟这个人有关的关系。它是定妆的依据（想复仇的人和想赎罪的人
               眼神不一样），原来横在整面墙上头——依据该挨着它服务的那个人。 -->
          <div v-if="relsOf(openChar).length" class="rels">
            <div v-for="(r, i) in relsOf(openChar)" :key="i" class="rel">
              <b class="rel__who">{{ r.a === openChar.name ? r.b : r.a }}</b>
              <span v-if="r.kind" class="pill pill--neutral tiny nowrap">{{ r.kind }}</span>
              <span v-if="r.tension" class="rel__why small">{{ r.tension }}</span>
            </div>
          </div>
        <div class="drawer__body">
          <div class="chr__cols">
            <!-- 外观 -->
            <div class="stack">
              <label class="field">
                <span class="field__label">称呼</span>
                <input v-model="edits[openChar.char_id].name" class="input" />
              </label>

              <label v-for="f in FIELDS" :key="f.key" class="field" :title="f.hint">
                <span class="field__label">{{ f.label }}</span>
                <textarea
                  v-model="edits[openChar.char_id][f.key]"
                  class="textarea textarea--tight"
                  :rows="f.rows"
                  :placeholder="f.hint"
                />
              </label>

              <!-- **折起来。** 这是排障用的——出来的图不对时，翻开看一眼
                   真正发给画图模型的那一串。平时它是一整段灰字，白占抽屉里
                   三分之一的高度，而抽屉是用来改描述的。 -->
              <details class="fold" title="每个镜头拿到的都是这一串，逐字节相同">
                <summary class="fold__t">拼出来的提示词</summary>
                <p class="rendered mono">{{ openChar.rendered }}</p>
              </details>
            </div>

            <!-- 参考图与音色 -->
            <div class="stack">
              <div class="field">
                <span class="field__label">
                  三视图参考
                  <button
                    class="btn btn--sm btn--ai"
                    type="button"
                    :disabled="isBusy('genall:' + openChar.char_id)"
                    title="照左边那段拼出来的提示词画。一张几十秒，三张一张一张来"
                    @click="genAllRefs(openChar.char_id)"
                  >
                    <AppIcon name="sparkle" :size="13" />
                    {{
                      isBusy('genall:' + openChar.char_id)
                        ? `正在画${genStep?.label ?? ''}${genStep?.pct ? ' ' + genStep.pct + '%' : '…'}`
                        : '三张一起画'
                    }}
                  </button>
                </span>
                <div class="refs">
                  <div v-for="s in SLOTS" :key="s.key" class="ref">
                    <div class="ref__frame">
                      <img
                        v-if="openChar['ref_' + s.key]"
                        :src="mediaUrl(session.projectPath, openChar['ref_' + s.key])"
                        :alt="s.label"
                      />
                      <AppIcon v-else name="image" :size="18" />
                    </div>
                    <span class="ref__label tiny">{{ s.label }}</span>
                    <div class="ref__acts">
                      <button
                        class="btn btn--sm btn--ai"
                        type="button"
                        :disabled="isBusy('gen:' + openChar.char_id + s.key) || isBusy('genall:' + openChar.char_id)"
                        :title="openChar['ref_' + s.key] ? '重画这一张（同一个种子，还是那张脸）' : '照提示词画一张'"
                        @click="genRef(openChar.char_id, s.key)"
                      >
                        <!-- 画着的时候把百分比写出来。**一张几十秒**，
                             一句不动的「画着…」分不清是在画还是卡住了；
                             而头十几秒还在把模型读进显存，那段时间一步都
                             不会推——所以没数的时候仍然显示「画着…」。 -->
                        {{
                          isBusy('gen:' + openChar.char_id + s.key)
                            ? genPct[openChar.char_id + s.key]
                              ? genPct[openChar.char_id + s.key] + '%'
                              : '画着…'
                            : '画'
                        }}
                      </button>
                      <label class="btn btn--sm btn--ghost">
                        {{ openChar['ref_' + s.key] ? '换' : '传' }}
                        <input
                          type="file"
                          accept="image/png,image/jpeg,image/webp"
                          hidden
                          @change="upload(openChar.char_id, s.key, $event)"
                        />
                      </label>
                      <button
                        v-if="openChar['ref_' + s.key]"
                        class="btn btn--sm btn--ghost"
                        type="button"
                        @click="clearRef(openChar.char_id, s.key)"
                      >
                        撤
                      </button>
                    </div>
                  </div>
                </div>
              </div>

              <label class="field">
                <span class="field__label">
                  音色
                  <span v-if="openChar.voice_gender" class="pill pill--neutral tiny">
                    猜的性别：{{ openChar.voice_gender === 'female' ? '女' : '男' }}
                  </span>
                </span>
                <!-- **手填的那一栏留着，但它不再是唯一的路。**
                     进程内配音的音色来自一段参考音频，外部服务要的是那个
                     服务认的音色名——两种都得能填，所以输入框不能换成
                     下拉框。但"只有一个空输入框"等于要用户手打路径，
                     打错的表现只是配音失败（2026-09-13 用户提的）。
                     所以下面补两件事：把项目里已有的片段列出来能点，
                     以及直接传一段进去。 -->
                <input
                  v-model="edits[openChar.char_id].voice_id"
                  class="input mono"
                  :list="voices.length ? 'voices-' + openChar.char_id : undefined"
                  placeholder="留空 = 自动挑（按性别和中文样本）"
                />
                <datalist v-if="voices.length" :id="'voices-' + openChar.char_id">
                  <option v-for="v in voices" :key="v" :value="v" />
                </datalist>

                <div class="row row--wrap">
                  <label class="btn btn--sm btn--ghost">
                    {{ edits[openChar.char_id].voice_id ? '换一段' : '传一段人声' }}
                    <input
                      type="file"
                      accept="audio/wav,audio/x-wav,audio/mpeg,audio/mp4,audio/flac"
                      hidden
                      @change="uploadVoice(openChar.char_id, $event)"
                    />
                  </label>
                  <button
                    class="btn btn--sm btn--ghost"
                    type="button"
                    :disabled="!edits[openChar.char_id].voice_id || isBusy('say:' + openChar.char_id)"
                    @click="tryVoice(openChar.char_id)"
                  >
                    {{ isBusy('say:' + openChar.char_id) ? '合成中…' : '试听' }}
                  </button>
                  <button
                    v-if="edits[openChar.char_id].voice_id"
                    class="btn btn--sm btn--ghost"
                    type="button"
                    @click="clearVoice(openChar.char_id)"
                  >
                    撤
                  </button>
                </div>

                <!-- 项目里已经有的几段，点一下就换过去。**这就是这一版的
                     "音色清单"**：没有服务端列表，有的是这个项目存了哪几段。
                     用 btn 那套而不是 chip：chip 的样式 scoped 在 EpShots 里，
                     搬过来只会是一排没样式的裸按钮。 -->
                <div v-if="voices.length" class="row row--wrap">
                  <button
                    v-for="v in voices"
                    :key="v"
                    class="btn btn--sm"
                    :class="
                      edits[openChar.char_id].voice_id === v
                        ? 'btn--primary'
                        : 'btn--ghost'
                    "
                    type="button"
                    :title="v"
                    @click="edits[openChar.char_id].voice_id = v"
                  >
                    {{ v.replace(/^voices\//, '') }}
                  </button>
                </div>

                <span v-if="!voicesLoading && voicesError" class="tiny dim">
                  {{ voicesError }}
                </span>

                <!-- 制作音色。**摇不是描述**，理由见脚本里那一段。 -->
                <details class="voice-make">
                  <summary class="tiny">制作一个新音色</summary>
                  <p class="tiny dim">
                    不给参考音频时，说话人是随机摇出来的——摇到喜欢的存下来，
                    从此这个声音就定死了。
                  </p>
                  <div class="row row--wrap">
                    <button
                      class="btn btn--sm btn--primary"
                      type="button"
                      :disabled="isBusy('take')"
                      @click="rollVoice()"
                    >
                      {{ isBusy('take') ? '摇着…' : take ? '再摇一个' : '摇一个' }}
                    </button>
                    <button
                      v-if="take"
                      class="btn btn--sm btn--ghost"
                      type="button"
                      @click="playRel(take.rel)"
                    >
                      再听一遍
                    </button>
                    <span v-if="take" class="tiny dim numeric">
                      种子 {{ take.seed }}
                      <template v-if="take.hz"> · 基频约 {{ take.hz }} Hz</template>
                    </span>
                  </div>

                  <!-- 预置音色就是几个固定的种子：每个人在每台机器上摇到的
                       是同一批人。点一下就摇那一个。 -->
                  <div v-if="presets.length" class="row row--wrap">
                    <!-- 基频是摇出来量的：男声大致 85~180 Hz、女声 165~255。
                         标它比编一个"沉稳中年男"诚实，而且点之前就能挑。 -->
                    <span class="tiny dim">预置（按音高排）：</span>
                    <button
                      v-for="p in presets"
                      :key="p.id"
                      class="btn btn--sm btn--ghost"
                      type="button"
                      :disabled="isBusy('take')"
                      :title="'种子 ' + p.seed + (p.hz ? '，上次量到约 ' + p.hz + ' Hz' : '')"
                      @click="rollVoice(p.seed)"
                    >
                      {{ p.name }}<span v-if="p.hz" class="dim"> · {{ p.hz }}Hz</span>
                    </button>
                  </div>

                  <div v-if="take" class="row row--wrap">
                    <input
                      v-model="takeName"
                      class="input"
                      placeholder="给它起个名，比如 低沉男声"
                    />
                    <button
                      class="btn btn--sm btn--primary"
                      type="button"
                      :disabled="isBusy('savetake')"
                      @click="saveTake(openChar.char_id)"
                    >
                      {{ isBusy('savetake') ? '存着…' : '存成音色' }}
                    </button>
                  </div>
                  <p v-if="take" class="tiny dim">
                    存的时候会按同一个种子重出一段更长的——参考音频越长，
                    克隆越稳。
                  </p>
                </details>
              </label>

              <label class="field">
                <span class="field__label">LoRA 触发词</span>
                <input
                  v-model="edits[openChar.char_id].lora_trigger"
                  class="input mono"
                  placeholder="训了角色 LoRA 才填"
                />
              </label>
            </div>
          </div>

          <div class="chr__foot">
            <button
              class="btn btn--primary btn--sm"
              type="button"
              :disabled="!changed(openChar.char_id) || isBusy('save:' + openChar.char_id)"
              title="改了外观，已渲染的镜头会退回重跑"
              @click="save(openChar.char_id)"
            >
              {{ isBusy('save:' + openChar.char_id) ? '存着…' : '保存' }}
            </button>
            <button
              class="btn btn--ghost btn--sm"
              type="button"
              :disabled="!changed(openChar.char_id)"
              @click="edits[openChar.char_id] = { ...c }"
            >
              撤销
            </button>
          </div>
        </div>
        </aside>
      </div>

  </div>
</template>

<style scoped>
/* 制作音色那一块。默认收起来——大多数时候用户只是想挑一个已有的，
   摇音色是偶尔才做的事。 */
.voice-make {
  margin-top: 6px;
  padding: 8px 10px;
  border: 1px solid var(--line);
  border-radius: 8px;
}
.voice-make > summary {
  cursor: pointer;
  user-select: none;
}
.voice-make > * + * {
  margin-top: 6px;
}

.chars {
  display: flex;
  flex-direction: column;
  gap: var(--s2);
}
/* 一人一张牌，和「这一集」那面镜头墙一个样子。 */
.wall {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(230px, 1fr));
  gap: var(--s3);
  align-items: start;
}
.cell {
  border: 1px solid var(--line);
  border-radius: var(--r);
  overflow: hidden;
  background: var(--surface);
}
.cell--live { border-color: var(--accent); }
/* 三张全空的牌缩一半高。三个占位符不需要 9:16 的框——这一页真正要扫的
   是"哪些还没画"，而空牌越矮，有图的越显眼。 */
.cell--flat .trio {
  aspect-ratio: auto;
  height: 72px;
}
/* 抽屉开着的时候，墙上那张牌描一圈——不然一屏牌子长得一样，
   收起抽屉之后找不回刚才改的是哪个。 */
.cell--open { outline: 2px solid var(--accent); }

/* 三张各占三分之一，鼠标放上去那张摊开。 */
.trio {
  display: flex;
  width: 100%;
  aspect-ratio: 9 / 16;
  background: var(--bg-sunken);
  cursor: pointer;
}
.trio__one {
  position: relative;
  flex: 1 1 0;
  min-width: 0;
  overflow: hidden;
  display: grid;
  place-items: center;
  color: var(--text-3);
  border-right: 1px solid var(--bg);
  /* **摊开要快。** 这是个探查动作：鼠标扫过三张看是不是同一个人，
     慢吞吞地展开会把"扫一眼"变成"等一下"。 */
  transition: flex-grow 0.18s ease;
}
.trio__one:last-child { border-right: 0; }
/* 鼠标放上去的那张摊开成整格，另外两张让位。
   **用 :hover 不用 JS**：这一层没有状态，交给 CSS 比在组件里记一个
   hoverId 省一整条更新链路。 */
/* 让位的那两张留一条窄边，不是缩没：那条边是回去的路，
   十几个像素点不着，等于摊开之后只能靠移出去才收回来。 */
.trio:hover .trio__one { flex-grow: 0.55; }
/* ⚠️ **摊开那条要写成后代选择器，不能只写 `.trio__one:hover`。**
   scoped 样式会把 `[data-v-xxx]` 加在**最后一段**上：
       .trio:hover .trio__one  →  .trio:hover .trio__one[data-v]   （0,4,0）
       .trio__one:hover        →  .trio__one:hover[data-v]         （0,3,0）
   于是让位那条反而更具体，三张一起变成 0.55——还是均分，看着就是
   "鼠标放上去什么都没发生"。不加 scoped 的话两条同权重、靠先后顺序，
   写法看起来没毛病，所以这个坑只在真页面上才露出来。 */
.trio:hover .trio__one:hover { flex-grow: 5; }
.trio__one img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.trio__one.is-empty {
  background: repeating-linear-gradient(
    45deg, transparent, transparent 6px,
    color-mix(in srgb, var(--line) 40%, transparent) 6px,
    color-mix(in srgb, var(--line) 40%, transparent) 12px);
}
.trio__preview {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  object-fit: cover;
  pointer-events: none;
}
.trio__pct {
  position: absolute;
  left: 50%;
  top: 50%;
  transform: translate(-50%, -50%);
  padding: 1px 6px;
  border-radius: 999px;
  background: color-mix(in srgb, var(--bg) 75%, transparent);
  color: var(--accent);
  font-size: var(--fs-xs);
  pointer-events: none;
}
/* 位置名只在摊开的那张上写出来：三张挤着的时候写不下，
   而摊开之后正需要知道现在看的是哪一面。 */
.trio__label {
  position: absolute;
  left: 4px;
  bottom: 4px;
  padding: 0 4px;
  border-radius: var(--r-sm);
  background: color-mix(in srgb, var(--bg) 70%, transparent);
  color: var(--text-2);
  opacity: 0;
  transition: opacity 0.18s ease;
  pointer-events: none;
  white-space: nowrap;
}
.trio__one:hover .trio__label { opacity: 1; }

.cell__bottom {
  position: relative;
  display: flex;
  align-items: center;
  gap: 4px;
  padding: 4px 6px;
  border-top: 1px solid var(--line);
}
/* 进度铺成这一行的底色。见镜头墙里同名那条。 */
.cell__fill {
  position: absolute;
  inset: 0 auto 0 0;
  background: var(--accent-soft);
  pointer-events: none;
}
.cell__fill--idle {
  right: 0;
  animation: cell-sweep 1.4s ease-in-out infinite;
}
@keyframes cell-sweep {
  0%, 100% { opacity: 0.25; }
  50% { opacity: 0.7; }
}
.cell__name {
  position: relative;
  border: 0;
  background: transparent;
  color: var(--text-1);
  font-size: var(--fs-sm);
  font-weight: 600;
  cursor: pointer;
  padding: 0;
  min-width: 0;
}
.cell__bottom .pill {
  position: relative;
}

/* 抽屉里的关系那几行 */
.rels {
  display: grid;
  gap: var(--s1);
  padding: var(--s2) var(--s3);
  border-bottom: 1px solid var(--line);
}
.rel {
  display: flex;
  align-items: baseline;
  gap: var(--s2);
  flex-wrap: wrap;
  font-size: var(--fs-sm);
}
.rel__who { flex: none; }
.rel__why { color: var(--text-2); min-width: 0; }

/* 搜索框不该和按钮抢地方：够打四五个字就行 */
.find {
  width: 7rem;
  padding: 2px 8px;
  font-size: var(--fs-xs);
}

/* 抽屉。和「这一集」那面墙同一套尺寸，改一处两边就该一起改。 */
.drawer {
  position: fixed;
  inset: 0;
  z-index: 40;
  background: color-mix(in srgb, black 45%, transparent);
  display: flex;
  justify-content: flex-end;
}
.drawer__panel {
  display: flex;
  flex-direction: column;
  /* 比镜头墙那个宽一些：这儿一屏要放下六段外观描述加三张参考图，
     460px 下每个输入框只剩二十来个字宽，写「五官定型」那一段不够看。 */
  width: min(96vw, 720px);
  height: 100%;
  background: var(--surface);
  border-left: 1px solid var(--line);
}
.drawer__head {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3);
  border-bottom: 1px solid var(--line);
}
.drawer__body {
  flex: 1;
  overflow-y: auto;
  padding: var(--s3);
}
/* 抽屉里**上下排，不左右分栏**。七百多像素要塞下六段外观描述加三张参考图，
   分成两栏之后每栏三百出头——输入框只剩二十来个字宽，参考图更是三张挤在
   一起看不清脸。而这里恰恰是拿来看脸的。 */
.drawer .chr__cols {
  grid-template-columns: minmax(0, 1fr);
  gap: var(--s4);
}
/* **参考图排到最上面。** 上下排之后，一进来先看到的应该是那三张脸——
   这一页的活就是"看看像不像、不像就重画"，而外观那几段文字是改的时候
   才要读的。左右分栏时两边都在视野里，上下排就得挑一个先看。 */
.drawer .chr__cols > :nth-child(2) { order: -1; }
.chr__cols {
  display: grid;
  grid-template-columns: minmax(0, 1.15fr) minmax(0, 1fr);
  gap: var(--s6);
}
/* 保存条钉在视口底部。展开的角色编辑器比屏幕高，保存按钮在最下面，
   改完上面几段外观得先滚到底才找得到——那正是「以为存不上」的来源。 */
.chr__foot {
  position: sticky;
  bottom: 0;
  z-index: 4;
  display: flex;
  align-items: center;
  gap: var(--s2);
  flex-wrap: wrap;
  margin-top: var(--s4);
  padding: var(--s2) 0;
  border-top: 1px solid var(--line);
  background: color-mix(in srgb, var(--surface) 94%, transparent);
  backdrop-filter: blur(8px);
}

/* 折起来的排障块。summary 默认是 list-item，带个三角；留着——那个三角
   正是"这里还有东西"的唯一提示。 */
.fold__t {
  color: var(--text-3);
  font-size: var(--fs-xs);
  cursor: pointer;
}
.fold[open] .fold__t {
  margin-bottom: 4px;
}
.fold--warn {
  align-self: flex-start;
}
.fold--warn p {
  max-width: 46rem;
  color: var(--text-2);
}
.rendered {
  margin: 0;
  padding: var(--s3);
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
  color: var(--accent);
  line-height: 1.7;
  word-break: break-word;
}

.refs {
  display: grid;
  /* **minmax(0, 1fr) 不是 1fr。** `1fr` 的最小值是内容的最小宽度，而每一格
     底下那排「画 / 换 / 撤」撑着一个下限——于是三格加起来比容器还宽，
     第三张被切掉一半，而且是悄悄切的（外层横向滚动条在抽屉里看不见）。 */
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: var(--s3);
}
.ref {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 4px;
}
.ref__frame {
  width: 100%;
  aspect-ratio: 3 / 4;
  display: grid;
  place-items: center;
  border-radius: var(--r);
  border: 1px dashed var(--line-strong);
  background: var(--bg-sunken);
  color: var(--text-3);
  overflow: hidden;
}
.ref__frame img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.ref__label {
  color: var(--text-3);
}
.ref__acts {
  display: flex;
  gap: 4px;
}

@media (max-width: 900px) {
  .chr__cols {
    grid-template-columns: 1fr;
    gap: var(--s5);
  }
  .chr__desc {
    display: none;
  }
}
@media (max-width: 640px) {
  .chr__head .pill {
    display: none;
  }
}
</style>
