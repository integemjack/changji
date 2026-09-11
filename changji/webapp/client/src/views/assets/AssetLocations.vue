<script setup>
/**
 * 第四步：场景。分集阶段的第一步。
 *
 * 场景库是全剧共用的一份——分镜表里只存 location_id，空间和光线的描述
 * 由程序从库里拼接，逐字节相同。每集各存一份拷贝的话，同一个安保室在
 * 第一集和第五集会长得不一样，而这正是这套系统要防的事。
 *
 * 所以这一页不是「这一集自己的场景表」，而是把镜头对准库里属于这一集的
 * 那几个：出分镜之前，AI 按这一集的剧本补新场景；出了分镜之后，按镜头
 * 实际引用的 id 分成「本集用到」和「其他集的」两组。
 */
import { computed, reactive, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { useAction } from '@/composables/useAction'
import { runAsyncJob } from '@/composables/useAsyncJob'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

/** 每个场景画到百分之几。见 AssetCharacters 里同名那个。 */
const genPct = reactive({})

const assets = ref(null)
const shots = ref([])
const loading = ref(false)
const edits = ref({})
const showOthers = ref(false)

const FIELDS = [
  { key: 'space', label: '空间', hint: '房间大小、家具布置、镜头能看到什么', rows: 3 },
  { key: 'lighting', label: '光线', hint: '冷调顶光、暖调侧逆光', rows: 2 },
  { key: 'palette', label: '色彩', hint: '主色和点缀色，可留空', rows: 2 },
]

const locations = computed(() => assets.value?.locations ?? [])

const knownIds = computed(() => new Set(locations.value.map((l) => l.location_id)))

/**
 * 这一集的镜头引用了哪几个场景，各用了几镜。
 *
 * 两个字段都认：schema 里 scene_id 和 location_id 是分开的，模型十次有
 * 八次只填了 scene_id。那种镜头渲染时拿不到场景描述——不报错，只是画面
 * 里的房间每镜都不一样。下面的 unlinked 就是用来把这件事说出来的。
 */
const usage = computed(() => {
  const map = new Map()
  for (const s of shots.value) {
    const id = s.location_id || (knownIds.value.has(s.scene_id) ? s.scene_id : '')
    if (!id) continue
    map.set(id, (map.get(id) ?? 0) + 1)
  }
  return map
})

const unlinked = computed(
  () =>
    shots.value.filter((s) => !s.location_id && knownIds.value.has(s.scene_id)).length,
)

async function linkLocations() {
  const result = await run(
    () =>
      api.linkLocations({
        project: session.projectPath,
        episode_id: session.episodeId,
      }),
    { key: 'link', refresh: true },
  )
  if (!result) return
  ui.ok(
    `${result.linked} 个镜头接回了场景` +
      (result.reset_shots ? `，${result.reset_shots} 个镜头退回重跑` : ''),
  )
  await load()
}

// 还没出分镜的时候无从知道这一集用哪几个，全都算本集的
const hasShots = computed(() => shots.value.length > 0)
const mine = computed(() =>
  hasShots.value
    ? locations.value.filter((l) => usage.value.has(l.location_id))
    : locations.value,
)
const others = computed(() =>
  hasShots.value
    ? locations.value.filter((l) => !usage.value.has(l.location_id))
    : [],
)

/** 分镜引用了、但库里没有的场景。跑起来会直接报「场景未注册」。 */
const missing = computed(() => {
  const known = new Set(locations.value.map((l) => l.location_id))
  return [...usage.value.keys()].filter((id) => !known.has(id))
})

async function load() {
  if (!session.projectPath) return
  loading.value = true
  try {
    assets.value = await api.assets(session.projectPath)
    edits.value = Object.fromEntries(
      (assets.value.locations ?? []).map((l) => [l.location_id, { ...l }]),
    )
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
  }
  await loadShots()
}

async function loadShots() {
  if (!session.projectPath || !session.episodeId) {
    shots.value = []
    return
  }
  try {
    const data = await api.shots(session.projectPath, session.episodeId)
    shots.value = data.shots ?? []
  } catch {
    shots.value = []
  }
}

watch(() => [session.projectPath, session.episodeId], load, { immediate: true })

function changed(id) {
  const now = edits.value[id]
  const was = locations.value.find((l) => l.location_id === id)
  if (!now || !was) return false
  return ['name', ...FIELDS.map((f) => f.key)].some(
    (k) => (now[k] ?? '') !== (was[k] ?? ''),
  )
}

/**
 * 照故事给这一集的地方定妆。
 *
 * **这一步不创作新的地方**——地方是故事里定的，这里只是把故事里那份名单
 * 翻成"长什么样"：空间结构、光线基调、色彩方案。
 *
 * 默认只补还没定过妆的：库里已有的同名场景保住，手改过的描述和传过的
 * 空景图都还在。勾了覆盖才让新出的顶掉，那会把全剧镜头退回重跑。
 */
async function generate(overwrite) {
  if (!session.episodeId) {
    ui.warn('先在上面挑一集')
    return
  }
  if (overwrite && !confirm('覆盖会冲掉手改过的场景描述和空景图，全剧镜头也要重跑。继续？')) {
    return
  }
  const result = await run(
    () =>
      api.makeBible({
        project: session.projectPath,
        episode_id: session.episodeId,
        overwrite,
      }),
    { key: 'bible', refresh: true },
  )
  if (!result) return
  const added = result.added_locations?.length ?? 0
  ui.ok(
    added
      ? `补了 ${added} 个新场景：${result.added_locations.join('、')}`
      : '这一集的场景库里都有了，没补新的',
  )
  await load()
}

async function save(id) {
  const draft = edits.value[id]
  const patch = {}
  for (const key of ['name', ...FIELDS.map((f) => f.key)]) {
    if (draft[key] !== undefined && draft[key] !== null) patch[key] = draft[key]
  }
  const result = await run(
    () => api.saveLocation({ project: session.projectPath, location_id: id, patch }),
    { key: 'save:' + id, refresh: true },
  )
  if (!result) return
  ui.ok(result.reset_shots ? `已保存，${result.reset_shots} 个镜头退回重跑` : '已保存')
  await load()
}

async function uploadEmpty(locationId, event) {
  const file = event.target.files?.[0]
  if (!file) return
  const form = new FormData()
  form.append('project', session.projectPath)
  form.append('location_id', locationId)
  form.append('file', file)
  const result = await run(() => api.uploadLocationReference(form), {
    key: 'up:' + locationId,
  })
  event.target.value = ''
  if (!result) return
  ui.ok(`空景图已存（${result.size_kb} KB），${result.reset_shots} 个镜头退回重跑`)
  await load()
}

/**
 * 照着这个场景那段"拼出来的提示词"画一张空景图。
 *
 * **这是这一页上唯一真正在用 AI 的地方**：地方是故事里定的，这一页只把它
 * 翻成"长什么样"，再照着画出来。
 *
 * 画出来的**里面没有人**——空景图是场景一致性的锚点，后面每一镜都拿它当
 * 底子。里面站个人的话，那个人会被当成这个地方的一部分一路复制下去，而
 * 那是个不属于任何角色、也没法改的人。
 */
async function genEmpty(locationId) {
  const result = await run(
    () =>
      runAsyncJob(
        (extra) =>
          api.generateLocationReference({
            project: session.projectPath,
            location_id: locationId,
            ...extra,
          }),
        {
          prefix: 'ref',
          onProgress: (cur, total) => {
            genPct[locationId] = total > 0 ? Math.round((cur / total) * 100) : 0
          },
        },
      ),
    { key: 'gen:' + locationId },
  )
  delete genPct[locationId]
  if (!result) return
  ui.ok(`空景图画好了（${Math.round(result.seconds)} 秒）`)
  await load()
}

async function clearEmpty(locationId) {
  const result = await run(
    () =>
      api.clearLocationReference({
        project: session.projectPath,
        location_id: locationId,
      }),
    { key: 'clr:' + locationId },
  )
  if (result) {
    ui.ok('已撤掉空景图')
    await load()
  }
}
</script>

<template>
  <div class="locs">
    <!-- 场景库是全剧共用的一份，分镜表里只存 id。这一页按「本集用到的」分组。
         工具行：刷新、重定、照故事定。 -->
    <div class="toolbar">
      <span class="tiny dim">{{ hasShots ? '本集用到' : '场景库' }} {{ mine.length }} 个</span>
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
        v-if="locations.length"
        class="btn btn--ghost btn--sm"
        type="button"
        :disabled="!session.episodeId || isBusy('bible')"
        title="同名场景用新出的顶掉旧的，手改过的描述和空景图会丢"
        @click="generate(true)"
      >
        全部重新定妆
      </button>
      <button
        class="btn btn--ai btn--sm"
        type="button"
        :disabled="!session.episodeId || isBusy('bible')"
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
      <div v-if="unlinked" class="alert alert--warn">
        <AppIcon name="warn" :size="14" />
        <span title="只填了 scene_id 没接到场景的镜头，渲染时拿不到空间和光线">
          <b class="numeric">{{ unlinked }}</b> 个镜头没接到场景
        </span>
        <button
          class="btn btn--sm"
          type="button"
          :disabled="isBusy('link')"
          @click="linkLocations"
        >
          {{ isBusy('link') ? '接上中…' : '一键接上' }}
        </button>
      </div>

      <p v-if="missing.length" class="alert alert--bad">
        <AppIcon name="warn" :size="14" />
        <span>分镜引用了 {{ missing.join('、') }}，场景库里没有</span>
      </p>

      <EmptyState
        v-if="!loading && !locations.length"
        icon="scene"
        title="场景库还是空的"
        hint="先写故事，再点「照故事定妆」"
      >
        <RouterLink to="/story" class="btn btn--sm">去写故事</RouterLink>
      </EmptyState>

      <template v-else>
        <!-- 本集场景 -->
        <section>
          <div class="grid grid--locs">
            <article v-for="l in mine" :key="l.location_id" class="loc">
              <!-- 空景图 -->
              <div class="loc__shot">
                <img
                  v-if="l.ref_empty"
                  :src="mediaUrl(session.projectPath, l.ref_empty)"
                  :alt="`${l.name} 空景图`"
                  loading="lazy"
                />
                <div v-else class="loc__blank">
                  <AppIcon name="image" :size="20" />
                  <span class="tiny">没有空景图</span>
                </div>

                <div class="loc__overlay">
                  <button
                    class="btn btn--sm btn--ai"
                    type="button"
                    :disabled="isBusy('gen:' + l.location_id)"
                    title="照下面那段拼出来的提示词画一张空景，里面不会有人"
                    @click="genEmpty(l.location_id)"
                  >
                    <AppIcon name="sparkle" :size="13" />
                    {{
                      isBusy('gen:' + l.location_id)
                        ? genPct[l.location_id]
                          ? genPct[l.location_id] + '%'
                          : '画着…'
                        : l.ref_empty
                          ? '重画'
                          : '画一张'
                    }}
                  </button>
                  <label class="btn btn--sm">
                    <AppIcon name="image" :size="13" />
                    {{ l.ref_empty ? '换一张' : '传空景图' }}
                    <input
                      type="file"
                      accept="image/png,image/jpeg,image/webp"
                      hidden
                      @change="uploadEmpty(l.location_id, $event)"
                    />
                  </label>
                  <button
                    v-if="l.ref_empty"
                    class="btn btn--sm btn--ghost"
                    type="button"
                    @click="clearEmpty(l.location_id)"
                  >
                    撤掉
                  </button>
                </div>

                <span v-if="usage.get(l.location_id)" class="loc__count pill pill--neutral">
                  本集 {{ usage.get(l.location_id) }} 镜
                </span>
              </div>

              <div class="loc__head">
                <input v-model="edits[l.location_id].name" class="loc__name" />
                <span class="tiny dim mono nowrap">{{ l.location_id }}</span>
              </div>

              <div class="loc__body stack stack--sm">
                <label v-for="f in FIELDS" :key="f.key" class="field" :title="f.hint">
                  <span class="field__label">{{ f.label }}</span>
                  <textarea
                    v-model="edits[l.location_id][f.key]"
                    class="textarea textarea--tight"
                    :rows="f.rows"
                    :placeholder="f.hint"
                  />
                </label>
                <div class="field" title="每个镜头拿到的都是这一串">
                  <span class="field__label">拼出来的提示词</span>
                  <p class="rendered mono">{{ l.rendered }}</p>
                </div>
              </div>

              <div class="loc__foot">
                <button
                  class="btn btn--primary btn--sm"
                  type="button"
                  :disabled="!changed(l.location_id) || isBusy('save:' + l.location_id)"
                  title="改了描述，已渲染的镜头会退回重跑"
                  @click="save(l.location_id)"
                >
                  保存
                </button>
                <button
                  class="btn btn--ghost btn--sm"
                  type="button"
                  :disabled="!changed(l.location_id)"
                  @click="edits[l.location_id] = { ...l }"
                >
                  撤销
                </button>
                <span class="spacer" />
                <span v-if="changed(l.location_id)" class="pill pill--warn">未保存</span>
              </div>
            </article>
          </div>
        </section>

        <!-- 其他集的场景 -->
        <section v-if="others.length" class="stack stack--sm">
          <button
            class="fold"
            type="button"
            title="同一个库，这一集没用到"
            @click="showOthers = !showOthers"
          >
            <AppIcon :name="showOthers ? 'arrowLeft' : 'arrowRight'" :size="14" />
            <span>其他集的场景</span>
            <span class="tab__n">{{ others.length }}</span>
          </button>

          <div v-if="showOthers" class="grid grid--locs">
            <article v-for="l in others" :key="l.location_id" class="loc loc--dim">
              <div class="loc__shot loc__shot--slim">
                <img
                  v-if="l.ref_empty"
                  :src="mediaUrl(session.projectPath, l.ref_empty)"
                  :alt="l.name"
                  loading="lazy"
                />
                <div v-else class="loc__blank"><AppIcon name="image" :size="16" /></div>
              </div>
              <div class="loc__head">
                <span class="strong truncate">{{ l.name }}</span>
                <span class="tiny dim mono nowrap">{{ l.location_id }}</span>
              </div>
              <div class="loc__body">
                <p class="rendered mono">{{ l.rendered }}</p>
              </div>
            </article>
          </div>
        </section>
      </template>
  </div>
</template>

<style scoped>
.locs {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
}

/* 出了问题才有的一行。 */
.alert {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin: 0;
  font-size: var(--fs-sm);
}
.alert--bad {
  color: var(--danger);
}
.alert--warn {
  color: var(--warn);
}
.alert--warn b {
  font-weight: 700;
}

.grid--locs {
  grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
  align-items: start;
}

/* 一格一个场景。图在上，格子要有边，不然图和图连成一片。 */
.loc {
  overflow: hidden;
  border: 1px solid var(--line);
  border-radius: var(--r);
}
.loc__foot {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s2) var(--s3);
  border-top: 1px solid var(--line);
}
.loc--dim {
  opacity: 0.72;
}

/* 空景图。16:9 是勘景照的常见比例，成片是竖屏，但这里给人看空间，
   宽一点看得清家具位置。 */
.loc__shot {
  position: relative;
  aspect-ratio: 16 / 9;
  background: var(--bg-sunken);
  border-bottom: 1px solid var(--line);
  overflow: hidden;
}
.loc__shot--slim {
  aspect-ratio: 21 / 9;
}
.loc__shot img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.loc__blank {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 4px;
  color: var(--text-3);
}
.loc__overlay {
  position: absolute;
  inset: auto 0 0 0;
  display: flex;
  gap: var(--s2);
  padding: var(--s2);
  background: linear-gradient(transparent, rgba(0, 0, 0, 0.65));
  opacity: 0;
  transition: opacity 0.15s var(--ease);
}
.loc:hover .loc__overlay,
.loc:focus-within .loc__overlay {
  opacity: 1;
}
.loc__count {
  position: absolute;
  top: var(--s2);
  left: var(--s2);
  background: rgba(0, 0, 0, 0.6);
  color: #fff;
  border-color: transparent;
}

.loc__head {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3) var(--s4);
  border-bottom: 1px solid var(--line);
}
.loc__name {
  flex: 1;
  min-width: 0;
  padding: 2px var(--s2);
  border: 1px solid transparent;
  border-radius: var(--r-sm);
  background: transparent;
  font-size: var(--fs-md);
  font-weight: 600;
}
.loc__name:hover {
  border-color: var(--line);
}
.loc__name:focus {
  outline: none;
  border-color: var(--accent);
  background: var(--bg-sunken);
}
.loc__body {
  padding: var(--s4);
}

.textarea--tight {
  min-height: 0;
}
.rendered {
  margin: 0;
  padding: var(--s2) var(--s3);
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
  color: var(--accent);
  line-height: 1.7;
  word-break: break-word;
}

.fold {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: 6px 0;
  border: 0;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
  text-align: left;
}
.fold:hover {
  color: var(--text);
}
</style>
