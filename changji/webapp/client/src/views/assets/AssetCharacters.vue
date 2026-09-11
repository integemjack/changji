<script setup>
/**
 * 第三步：角色。
 *
 * 角色外观只存在这里，分镜表里只有 id。所以这一页改一个字，
 * 全剧几十个镜头的提示词都跟着变——引擎会把已渲染的镜头退回重跑，
 * 界面必须把这件事说在前面，别让人改完才发现成片全没了。
 */
import { computed, nextTick, onMounted, onUnmounted, reactive, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { useAction } from '@/composables/useAction'
import { runAsyncJob } from '@/composables/useAsyncJob'
import { useRefGen } from '@/composables/useRefGen'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

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
const genPct = reactive({})

/**
 * 采样到一半那张小图，按「角色+位置」记。
 *
 * 用户 2026-09-12：「画图方式也要实时返回步数图」。一张几十秒，头十几秒
 * 还在把模型读进显存，一个百分比撑不住这段等待——而这张图是潜空间线性
 * 投影来的（不走 VAE，几乎不花时间），第五步就看得出构图对不对，不对
 * 当场撤掉重来，不用等它画完。
 *
 * **画完就删**。真图上来之后再盖着一张糊的，比没有更糟。
 */
const preview = reactive({})

/** 页头那个种子，和「一键出图」画完之后的那声招呼。 */
const { stamp, seedPayload } = useRefGen()

/** 抽屉里改的是哪一个。 */
const openChar = computed(
  () => characters.value.find((c) => c.char_id === openId.value) ?? null,
)

/** 这一格在不在画。三张里任意一张在画，整张牌子就算在跑。 */
function cellBusy(charId) {
  if (isBusy('genall:' + charId)) return true
  return SLOTS.some((s) => isBusy('gen:' + charId + s.key))
}

/** 这一格画到百分之几。没数就回 null，界面画一条来回跑的条。 */
function cellPct(charId) {
  for (const s of SLOTS) {
    const v = genPct[charId + s.key]
    if (v) return v
  }
  return null
}

const assets = ref(null)
const loading = ref(false)
const openId = ref('')
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

const characters = computed(() => assets.value?.characters ?? [])
const refsUsed = computed(() => assets.value?.reference_images_used)

async function load() {
  if (!session.projectPath) return
  loading.value = true
  try {
    assets.value = await api.assets(session.projectPath)
    edits.value = Object.fromEntries(
      (assets.value.characters ?? []).map((c) => [c.char_id, { ...c }]),
    )
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
  }
}

function onEsc(e) {
  // 抽屉盖着半个屏幕，而鼠标多半正停在里面——Esc 是唯一不用先瞄准的出口。
  if (e.key === 'Escape' && openId.value) openId.value = ''
}
onMounted(() => document.addEventListener('keydown', onEsc))
onUnmounted(() => document.removeEventListener('keydown', onEsc))

watch(() => session.projectPath, load, { immediate: true })
// 页头的「一键出图」画完一张就招呼一声，这儿跟着重拉——不然图已经在
// 磁盘上了，界面还是一片空。
watch(stamp, load)

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
async function generate(overwrite) {
  if (overwrite && !confirm('覆盖会冲掉手改过的设定和传过的参考图，已渲染的镜头也要重跑。继续？')) {
    return
  }
  const result = await run(
    () => api.makeBible({ project: session.projectPath, overwrite }),
    { key: 'bible', refresh: true },
  )
  if (!result) return
  const added = result.added_characters?.length ?? 0
  ui.ok(
    added
      ? `补了 ${added} 个新角色：${result.added_characters.join('、')}`
      : '剧本里的人物库里都有了，没补新的',
  )
  await load()
}

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
  ui.ok(`参考图已存（${result.size_kb} KB），${result.reset_shots} 个镜头退回重跑`)
  await load()
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
            ...seedPayload(),
            ...extra,
          }),
        {
          prefix: 'ref',
          onProgress: (cur, total) => {
            genPct[charId + slot] = total > 0 ? Math.round((cur / total) * 100) : 0
          },
          onPreview: (url) => {
            preview[charId + slot] = url
          },
        },
      ),
    { key: 'gen:' + charId + slot },
  )
  delete genPct[charId + slot]
  delete preview[charId + slot]
  if (!result) return
  ui.ok(`${SLOTS.find((s) => s.key === slot)?.label ?? slot}画好了（${Math.round(result.seconds)} 秒）`)
  await load()
}

/** 三张一起画。**一张一张来**：显存只够一张，并发只会排队，还看不出进度。 */
async function genAllRefs(charId) {
  for (const s of SLOTS) {
    genStep.value = { charId, label: s.label }
    const ok = await run(
      () =>
        runAsyncJob(
          (extra) =>
            api.generateReference({
              project: session.projectPath,
              char_id: charId,
              slot: s.key,
              ...seedPayload(),
              ...extra,
            }),
          {
            prefix: 'ref',
            onProgress: (cur, total) => {
              genStep.value = {
                charId,
                label: s.label,
                pct: total > 0 ? Math.round((cur / total) * 100) : 0,
              }
              genPct[charId + s.key] = total > 0 ? Math.round((cur / total) * 100) : 0
            },
            onPreview: (url) => {
              preview[charId + s.key] = url
            },
          },
        ),
      { key: 'genall:' + charId },
    )
    delete genPct[charId + s.key]
    delete preview[charId + s.key]
    // 中间某一张失败就停：后面两张多半也会栽在同一件事上（模型没配、
    // 显存不够），接着画只是让用户多等两分钟再看到同一句报错。
    if (!ok) break
  }
  genStep.value = null
  await load()
}

async function clearRef(charId, slot) {
  const result = await run(
    () => api.clearReference({ project: session.projectPath, char_id: charId, slot }),
    { key: 'clr:' + charId + slot },
  )
  if (result) {
    ui.ok('已撤掉这张参考图')
    await load()
  }
}
</script>

<template>
  <div class="chars">
    <!-- 人是故事里定的，这一页只给他们定妆。工具行：刷新、重定、照故事定。 -->
    <div class="toolbar">
      <span class="tiny dim">{{ characters.length }} 人</span>
      <span class="spacer" />
      <button
        class="btn btn--ghost btn--sm"
        type="button"
        :disabled="loading || !session.hasProject"
        title="刷新"
        @click="load"
      >
        <AppIcon name="refresh" :size="14" />
      </button>
      <button
        v-if="characters.length"
        class="btn btn--ghost btn--sm"
        type="button"
        :disabled="isBusy('bible')"
        title="同名角色用新出的顶掉旧的，手改过的设定和参考图会丢"
        @click="generate(true)"
      >
        全部重新定妆
      </button>
      <button
        class="btn btn--ai btn--sm"
        type="button"
        :disabled="!session.hasProject || isBusy('bible')"
        @click="generate(false)"
      >
        <AppIcon name="sparkle" :size="14" />
        {{ isBusy('bible') ? '正在读故事…' : '照故事定妆' }}
      </button>
    </div>

    <!-- **不要在这儿套一个光秃秃的 <template>**：Vue 只把带
         v-if/v-for/v-slot 的 template 当片段，没有指令的会当成真的
         HTML template 元素渲染出去——浏览器默认 display:none，
         内容全在 DOM 里、一个字不报错，就是看不见。栽过一次。 -->
      <p v-if="refsUsed === false" class="tiny warn-text">{{ assets?.reference_hint }}</p>

      <EmptyState
        v-if="!loading && !characters.length"
        icon="user"
        title="还没有角色"
        hint="先写故事，再点「照故事定妆」"
      >
        <RouterLink to="/story" class="btn btn--sm">去写故事</RouterLink>
      </EmptyState>

      <!-- **一人一张牌，和「这一集」那面镜头墙一个样子。**
           用户 2026-09-12：「角色，场景的展示方式和这一集一样」。
           以前是一行一个人、头像只有三十几个像素——而这一页的产出就是图，
           把图做成一行里的小圆点，等于把要看的东西藏起来。 -->
      <div v-else class="wall">
        <article
          v-for="c in characters"
          :key="c.char_id"
          class="cell"
          :class="{ 'cell--live': cellBusy(c.char_id), 'cell--open': openId === c.char_id }"
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
                v-if="preview[c.char_id + s.key]"
                class="trio__preview"
                :src="preview[c.char_id + s.key]"
                alt=""
              />
              <span v-if="genPct[c.char_id + s.key]" class="trio__pct numeric">
                {{ genPct[c.char_id + s.key] }}%
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
            <span v-if="changed(c.char_id)" class="pill pill--warn tiny">未保存</span>
            <span class="pill tiny" :class="c.voice_id ? 'pill--ok' : 'pill--neutral'">
              {{ c.voice_id ? '音色' : '自动' }}
            </span>
            <span
              class="pill tiny"
              :class="SLOTS.filter((s) => c['ref_' + s.key]).length === 3 ? 'pill--ok' : 'pill--neutral'"
            >
              {{ SLOTS.filter((s) => c['ref_' + s.key]).length }}/3
            </span>
          </div>

        </article>
      </div>

      <!-- 点开一个角色，从右边滑出来改。**和「这一集」那面镜头墙一个做法**
           （用户 2026-09-12：「展示方式和这一集一样」）：墙是用来挑的，
           抽屉是用来改的——把编辑器塞回牌子里会把那一格撑成一整行，
           一墙的牌子跟着重排，而人刚刚就是靠位置认出那张牌的。 -->
      <div v-if="openChar" class="drawer" @click.self="openId = ''">
        <aside class="drawer__panel">
          <header class="drawer__head">
            <b>{{ openChar.name }}</b>
            <span class="tiny dim mono">{{ openChar.char_id }}</span>
            <span v-if="changed(openChar.char_id)" class="pill pill--warn tiny">未保存</span>
            <span class="spacer" />
            <button class="iconbtn" type="button" title="收起（Esc）" @click="openId = ''">
              ✕
            </button>
          </header>
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

              <div class="field" title="每个镜头拿到的都是这一串，逐字节相同">
                <span class="field__label">拼出来的提示词</span>
                <p class="rendered mono">{{ openChar.rendered }}</p>
              </div>
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
                <!-- **是输入框不是下拉框。** 拆掉 ComfyUI 之后音色不再是
                     服务端的一份清单：进程内配音要的是一段参考音频的路径，
                     外部服务要的是那个服务认的音色名。两种都得能手填——
                     留成下拉框的话，列表永远是空的，用户**根本填不进去**。
                     服务端真给了清单（将来某个后端支持）就走 datalist。 -->
                <input
                  v-model="edits[openChar.char_id].voice_id"
                  class="input mono"
                  :list="voices.length ? 'voices-' + openChar.char_id : undefined"
                  placeholder="留空 = 自动挑（按性别和中文样本）"
                />
                <datalist v-if="voices.length" :id="'voices-' + openChar.char_id">
                  <option v-for="v in voices" :key="v" :value="v" />
                </datalist>
                <span v-if="!voicesLoading && voicesError" class="tiny warn-text">
                  {{ voicesError }}
                </span>
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

.textarea--tight {
  min-height: 0;
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
