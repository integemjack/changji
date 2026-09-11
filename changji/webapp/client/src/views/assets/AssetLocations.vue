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
import { computed, ref, watch } from 'vue'

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
const shots = ref([])
const loading = ref(false)
const edits = ref({})
const showOthers = ref(false)

const FIELDS = [
  { key: 'space', label: '空间结构', hint: '房间大小、家具布置、镜头能看到什么。', rows: 3 },
  { key: 'lighting', label: '光线基调', hint: '如：冷调顶光、暖调侧逆光。同场景的镜头靠它统一。', rows: 2 },
  { key: 'palette', label: '色彩方案', hint: '主色和点缀色。可留空。', rows: 2 },
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
 * 按这一集的剧本补场景。
 *
 * 默认只补新的：库里已有的同名场景保住，手改过的描述和传过的空景图
 * 都还在。勾了覆盖才让新出的顶掉，那会把全剧镜头退回重跑。
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
  <div class="stack stack--lg">
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
          v-if="locations.length"
          class="btn btn--ghost"
          type="button"
          :disabled="!session.episodeId || isBusy('bible')"
          title="同名场景用新出的顶掉旧的，手改过的描述和空景图会丢"
          @click="generate(true)"
        >
          全部重出
        </button>
        <button
          class="btn btn--ai"
          type="button"
          :disabled="!session.episodeId || isBusy('bible')"
          @click="generate(false)"
        >
          <AppIcon name="sparkle" :size="15" />
          {{ isBusy('bible') ? '正在读剧本…' : 'AI 出这一集的场景' }}
        </button>
      </template>
      <template v-if="session.hasProject" #note>
        <p class="notice">
          <AppIcon name="info" :size="15" />
          <span>
            场景库是全剧共用的一份，分镜表里只存 id、描述由程序拼接——同一个地点在
            哪一集出现都长一样，靠的就是这个。这一页只是把它按「本集用到的」分了组。
          </span>
        </p>
      </template>
    </StepHeader>

    <!-- 「还没选项目」归父页面判，每块各判一遍是同一句话写两遍。
         「还没选到某一集」那条也去掉了：**场景库是全剧共用的**，
         站在这一页上不需要先挑一集。出场景那个按钮仍然按当前集的剧本走，
         没选集时它自己是禁用的。 -->

    <template>
      <div v-if="unlinked" class="alert alert--warn">
        <AppIcon name="warn" :size="15" />
        <span>
          这一集有 <b class="numeric">{{ unlinked }}</b> 个镜头只填了 scene_id，
          没接到场景上。这种镜头渲染时拿不到空间和光线的描述——不会报错，
          但同一个房间在每个镜头里都会长得不一样。
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
        <AppIcon name="warn" :size="15" />
        <span>
          分镜引用了 {{ missing.join('、') }}，但场景库里没有。这样跑到首帧那一步会
          直接报「场景未注册」。点上面的「AI 出这一集的场景」补上。
        </span>
      </p>

      <EmptyState
        v-if="!loading && !locations.length"
        icon="scene"
        title="场景库还是空的"
        hint="场景从剧本里提。先把这一集的剧本写好，再点上面的按钮，AI 会读剧本把地点列出来。"
      >
        <RouterLink to="/story" class="btn">回去写故事</RouterLink>
      </EmptyState>

      <template v-else>
        <!-- 本集场景 -->
        <section class="stack">
          <div class="row row--between">
            <div class="row">
              <h2 class="section-title">
                {{ hasShots ? '本集用到的场景' : '场景库' }}
              </h2>
              <span class="pill pill--accent">{{ mine.length }} 个</span>
              <span v-if="!hasShots" class="tiny dim">
                这一集还没出分镜，先把库里的都列出来
              </span>
            </div>
          </div>

          <div class="grid grid--locs">
            <article v-for="l in mine" :key="l.location_id" class="loc card">
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
                <label v-for="f in FIELDS" :key="f.key" class="field">
                  <span class="field__label">{{ f.label }}</span>
                  <textarea
                    v-model="edits[l.location_id][f.key]"
                    class="textarea textarea--tight"
                    :rows="f.rows"
                  />
                </label>
                <div class="field">
                  <span class="field__label">拼出来的提示词</span>
                  <p class="rendered mono">{{ l.rendered }}</p>
                </div>
              </div>

              <div class="card__foot">
                <button
                  class="btn btn--primary btn--sm"
                  type="button"
                  :disabled="!changed(l.location_id) || isBusy('save:' + l.location_id)"
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
        <section v-if="others.length" class="stack">
          <button class="fold" type="button" @click="showOthers = !showOthers">
            <AppIcon :name="showOthers ? 'arrowLeft' : 'arrowRight'" :size="14" />
            <span class="strong">其他集的场景</span>
            <span class="pill pill--neutral">{{ others.length }} 个</span>
            <span class="tiny dim">同一个库，这一集没用到。改了会影响用到它的那几集</span>
          </button>

          <div v-if="showOthers" class="grid grid--locs">
            <article v-for="l in others" :key="l.location_id" class="loc loc--dim card">
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
    </template>
  </div>
</template>

<style scoped>
.section-title {
  font-size: var(--fs-lg);
  font-weight: 600;
}

.notice {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
  padding: var(--s3) var(--s4);
  border-radius: var(--r);
  background: var(--info-soft);
  border: 1px solid color-mix(in srgb, var(--info) 28%, transparent);
  color: var(--text-2);
  font-size: var(--fs-base);
  line-height: 1.6;
}
.notice :deep(svg) {
  color: var(--info);
  margin-top: 3px;
}

.alert {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
  padding: var(--s3) var(--s4);
  border-radius: var(--r);
  font-size: var(--fs-base);
  line-height: 1.6;
}
.alert--bad {
  background: var(--danger-soft);
  color: var(--danger);
}
.alert--warn {
  align-items: center;
  background: var(--warn-soft);
  color: var(--warn);
  border: 1px solid color-mix(in srgb, var(--warn) 32%, transparent);
}
.alert--warn span {
  flex: 1;
}
.alert--warn b {
  font-weight: 700;
}

.grid--locs {
  grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
  align-items: start;
}

.loc {
  overflow: hidden;
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
  width: 100%;
  padding: var(--s3) var(--s4);
  border: 1px solid var(--line);
  border-radius: var(--r);
  background: var(--surface);
  color: var(--text-2);
  font-size: var(--fs-base);
  cursor: pointer;
  text-align: left;
}
.fold:hover {
  border-color: var(--line-strong);
  color: var(--text);
}
</style>
