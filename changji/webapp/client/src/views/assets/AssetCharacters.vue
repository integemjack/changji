<script setup>
/**
 * 第三步：角色。
 *
 * 角色外观只存在这里，分镜表里只有 id。所以这一页改一个字，
 * 全剧几十个镜头的提示词都跟着变——引擎会把已渲染的镜头退回重跑，
 * 界面必须把这件事说在前面，别让人改完才发现成片全没了。
 */
import { computed, nextTick, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import StepHeader from '@/components/StepHeader.vue'
import { api, mediaUrl } from '@/api'
import { useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

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

watch(() => session.projectPath, load, { immediate: true })

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
    () => api.generateReference({ project: session.projectPath, char_id: charId, slot }),
    { key: 'gen:' + charId + slot },
  )
  if (!result) return
  ui.ok(`${SLOTS.find((s) => s.key === slot)?.label ?? slot}画好了（${Math.round(result.seconds)} 秒）`)
  await load()
}

/** 三张一起画。**一张一张来**：显存只够一张，并发只会排队，还看不出进度。 */
async function genAllRefs(charId) {
  for (const s of SLOTS) {
    genStep.value = { charId, label: s.label }
    const ok = await run(
      () => api.generateReference({ project: session.projectPath, char_id: charId, slot: s.key }),
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
  <div class="stack stack--lg">
    <h2 class="asec">
      人物
      <span class="asec__sub">
        人是故事里定的，这一页只给他们定妆：把故事里的人翻成"长什么样"，再照着画出参考图。
        外观只存在这儿，分镜表里只有 id——这是跨镜头一致的唯一手段。
      </span>
    </h2>

    <StepHeader bare>
      <template #actions>
        <button
          class="btn btn--ghost"
          type="button"
          :disabled="loading || !session.hasProject"
          @click="load"
        >
          <AppIcon name="refresh" :size="15" />
          刷新
        </button>
        <button
          v-if="characters.length"
          class="btn btn--ghost"
          type="button"
          :disabled="isBusy('bible')"
          title="同名角色用新出的顶掉旧的，手改过的设定和参考图会丢"
          @click="generate(true)"
        >
          全部重新定妆
        </button>
        <button
          class="btn btn--ai"
          type="button"
          :disabled="!session.hasProject || isBusy('bible')"
          @click="generate(false)"
        >
          <AppIcon name="sparkle" :size="15" />
          {{ isBusy('bible') ? '正在读故事…' : '照故事定妆' }}
        </button>
      </template>
    </StepHeader>

    <!-- 「还没选项目」归父页面判，每块各判一遍是同一句话写两遍 -->

    <!-- **不要在这儿套一个光秃秃的 <template>**：Vue 只把带
         v-if/v-for/v-slot 的 template 当片段，没有指令的会当成真的
         HTML template 元素渲染出去——浏览器默认 display:none，
         内容全在 DOM 里、一个字不报错，就是看不见。栽过一次。 -->
      <p v-if="refsUsed === false" class="notice">
        <AppIcon name="info" :size="15" />
        <span>{{ assets?.reference_hint }}</span>
      </p>

      <EmptyState
        v-if="!loading && !characters.length"
        icon="user"
        title="还没有角色"
        hint="人是故事里定的，这一页只给他们定妆。先去把故事写出来，再回来点「照故事定妆」。"
      >
        <RouterLink to="/story" class="btn">回去写故事</RouterLink>
        <button class="btn btn--ai" type="button" @click="generate(false)">
          <AppIcon name="sparkle" :size="15" />
          现在就定妆
        </button>
      </EmptyState>

      <div v-else class="stack">
        <article
          v-for="c in characters"
          :key="c.char_id"
          class="chr card"
          :class="{ 'chr--open': openId === c.char_id }"
          :data-char="c.char_id"
        >
          <button class="chr__head" type="button" @click="toggle(c.char_id)">
            <span class="chr__avatar">
              <img
                v-if="c.ref_front"
                :src="mediaUrl(session.projectPath, c.ref_front)"
                :alt="c.name"
              />
              <AppIcon v-else name="user" :size="20" />
            </span>
            <span class="chr__id">
              <span class="chr__name">{{ c.name }}</span>
              <span class="chr__meta tiny dim mono">{{ c.char_id }}</span>
            </span>
            <span class="chr__desc truncate muted small">{{ c.identity }}</span>
            <span class="spacer" />
            <span v-if="changed(c.char_id)" class="pill pill--warn">未保存</span>
            <span class="pill" :class="c.voice_id ? 'pill--ok' : 'pill--neutral'">
              {{ c.voice_id ? '已定音色' : '音色自动挑' }}
            </span>
            <span class="pill pill--neutral">
              {{ SLOTS.filter((s) => c['ref_' + s.key]).length }} / 3 参考图
            </span>
            <AppIcon
              class="chr__chev"
              :name="openId === c.char_id ? 'arrowLeft' : 'arrowRight'"
              :size="15"
            />
          </button>

          <div v-if="openId === c.char_id" class="chr__body">
            <div class="chr__cols">
              <!-- 外观 -->
              <div class="stack">
                <label class="field">
                  <span class="field__label">称呼</span>
                  <input v-model="edits[c.char_id].name" class="input" />
                </label>

                <label v-for="f in FIELDS" :key="f.key" class="field">
                  <span class="field__label">{{ f.label }}</span>
                  <textarea
                    v-model="edits[c.char_id][f.key]"
                    class="textarea textarea--tight"
                    :rows="f.rows"
                  />
                  <span class="field__hint">{{ f.hint }}</span>
                </label>

                <div class="field">
                  <span class="field__label">拼出来的提示词</span>
                  <p class="rendered mono">{{ c.rendered }}</p>
                  <span class="field__hint">
                    每个镜头拿到的都是这一串，逐字节相同。一致性就是这么来的。
                  </span>
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
                      :disabled="isBusy('genall:' + c.char_id)"
                      title="照左边那段拼出来的提示词画。一张几十秒，三张一张一张来"
                      @click="genAllRefs(c.char_id)"
                    >
                      <AppIcon name="sparkle" :size="13" />
                      {{
                        isBusy('genall:' + c.char_id)
                          ? `正在画${genStep?.label ?? ''}…`
                          : '三张一起画'
                      }}
                    </button>
                  </span>
                  <div class="refs">
                    <div v-for="s in SLOTS" :key="s.key" class="ref">
                      <div class="ref__frame">
                        <img
                          v-if="c['ref_' + s.key]"
                          :src="mediaUrl(session.projectPath, c['ref_' + s.key])"
                          :alt="s.label"
                        />
                        <AppIcon v-else name="image" :size="18" />
                      </div>
                      <span class="ref__label tiny">{{ s.label }}</span>
                      <div class="ref__acts">
                        <button
                          class="btn btn--sm btn--ai"
                          type="button"
                          :disabled="isBusy('gen:' + c.char_id + s.key) || isBusy('genall:' + c.char_id)"
                          :title="c['ref_' + s.key] ? '重画这一张（同一个种子，还是那张脸）' : '照提示词画一张'"
                          @click="genRef(c.char_id, s.key)"
                        >
                          {{ isBusy('gen:' + c.char_id + s.key) ? '画着…' : '画' }}
                        </button>
                        <label class="btn btn--sm btn--ghost">
                          {{ c['ref_' + s.key] ? '换' : '传' }}
                          <input
                            type="file"
                            accept="image/png,image/jpeg,image/webp"
                            hidden
                            @change="upload(c.char_id, s.key, $event)"
                          />
                        </label>
                        <button
                          v-if="c['ref_' + s.key]"
                          class="btn btn--sm btn--ghost"
                          type="button"
                          @click="clearRef(c.char_id, s.key)"
                        >
                          撤
                        </button>
                      </div>
                    </div>
                  </div>
                  <span class="field__hint">
                    文字描述再细，模型每次也会重新想象一遍这张脸；给一张图，它照着画。
                    画出来的和后面每一镜用的是同一段描述，所以是同一个人。
                  </span>
                </div>

                <label class="field">
                  <span class="field__label">
                    音色
                    <span v-if="c.voice_gender" class="pill pill--neutral tiny">
                      猜的性别：{{ c.voice_gender === 'female' ? '女' : '男' }}
                    </span>
                  </span>
                  <!-- **是输入框不是下拉框。** 拆掉 ComfyUI 之后音色不再是
                       服务端的一份清单：进程内配音要的是一段参考音频的路径，
                       外部服务要的是那个服务认的音色名。两种都得能手填——
                       留成下拉框的话，列表永远是空的，用户**根本填不进去**。
                       服务端真给了清单（将来某个后端支持）就走 datalist。 -->
                  <input
                    v-model="edits[c.char_id].voice_id"
                    class="input mono"
                    :list="voices.length ? 'voices-' + c.char_id : undefined"
                    placeholder="留空 = 自动挑（按性别和中文样本）"
                  />
                  <datalist v-if="voices.length" :id="'voices-' + c.char_id">
                    <option v-for="v in voices" :key="v" :value="v" />
                  </datalist>
                  <span v-if="voicesLoading" class="field__hint">
                    正在问有哪些音色…
                  </span>
                  <span v-else class="field__hint">
                    {{ voicesError || '留空也能跑，配音时按性别和中文样本挑。' }}
                  </span>
                </label>

                <label class="field">
                  <span class="field__label">LoRA 触发词</span>
                  <input
                    v-model="edits[c.char_id].lora_trigger"
                    class="input mono"
                    placeholder="训了角色 LoRA 才填"
                  />
                </label>
              </div>
            </div>

            <div class="chr__foot">
              <button
                class="btn btn--primary"
                type="button"
                :disabled="!changed(c.char_id) || isBusy('save:' + c.char_id)"
                @click="save(c.char_id)"
              >
                {{ isBusy('save:' + c.char_id) ? '保存中…' : '保存这个角色' }}
              </button>
              <button
                class="btn btn--ghost"
                type="button"
                :disabled="!changed(c.char_id)"
                @click="edits[c.char_id] = { ...c }"
              >
                撤销
              </button>
              <span class="spacer" />
              <span class="tiny dim">改了外观，已渲染的镜头会退回重跑</span>
            </div>
          </div>
        </article>
      </div>
  </div>
</template>

<style scoped>
/* 两块合成一页之后，光有两排按钮分不清哪排管人、哪排管地方。 */
.asec {
  display: flex;
  align-items: baseline;
  gap: var(--s3);
  flex-wrap: wrap;
  font-size: var(--fs-lg);
  font-weight: 700;
}
.asec__sub {
  font-size: var(--fs-sm);
  font-weight: 400;
  color: var(--text-3);
}

.notice {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
  padding: var(--s3) var(--s4);
  border-radius: var(--r);
  background: var(--info-soft);
  border: 1px solid color-mix(in srgb, var(--info) 30%, transparent);
  color: var(--text-2);
  font-size: var(--fs-base);
  line-height: 1.6;
}
.notice :deep(svg) {
  color: var(--info);
  margin-top: 3px;
}

.chr {
  overflow: hidden;
}
.chr--open {
  border-color: var(--accent-line);
}
.chr__head {
  display: flex;
  align-items: center;
  gap: var(--s3);
  width: 100%;
  padding: var(--s3) var(--s4);
  background: none;
  border: none;
  cursor: pointer;
  text-align: left;
}
.chr__head:hover {
  background: var(--surface-2);
}
.chr__avatar {
  flex: none;
  display: grid;
  place-items: center;
  width: 38px;
  height: 38px;
  border-radius: 12px;
  overflow: hidden;
  background: var(--surface-3);
  color: var(--text-3);
}
.chr__avatar img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.chr__id {
  display: flex;
  flex-direction: column;
  line-height: 1.25;
  flex: none;
}
.chr__name {
  font-weight: 600;
  font-size: var(--fs-md);
}
.chr__desc {
  max-width: 34ch;
}
.chr__chev {
  color: var(--text-3);
}

.chr__body {
  border-top: 1px solid var(--line);
  padding: var(--s5);
  background: var(--surface-2);
}
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
  margin: var(--s5) calc(var(--s5) * -1) calc(var(--s5) * -1);
  padding: var(--s3) var(--s5);
  border-top: 1px solid var(--line);
  background: color-mix(in srgb, var(--surface-2) 94%, transparent);
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
  grid-template-columns: repeat(3, 1fr);
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
  .chr__body {
    padding: var(--s4);
  }
}
</style>
