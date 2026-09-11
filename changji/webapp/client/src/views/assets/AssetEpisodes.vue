<script setup>
/**
 * 分集。设定三格里的第三格。
 *
 * 角色是「谁」，场景是「哪儿」，这一格是「怎么切」——同一个故事，每集
 * 30 秒切出来十几集，每集 3 分钟切出来三四集，讲的是同一件事。
 *
 * **这一页不写东西。** 正文在「故事」那一页，这里只看它被切成什么样：
 * 章节多长、线画在哪、每一集停在什么悬念上。往这里加编辑框的话，同一段
 * 正文就有两个地方能改，而两个地方迟早对不上。
 *
 * 分集是**章节之间那条线**，不是另一张表。单独列一张「第 3 集覆盖第 5~6
 * 章」的表，人看不见线画在哪，还得回去翻第 5 章是什么。
 */
import { computed, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

const story = ref(null)
const assets = ref(null)
const loading = ref(false)
const openChapter = ref('')

// **体量不在这一格。** 它是"这个故事有多长"，写大纲时就要定，属于创作，
// 所以留在「故事」那一页。这一格只管"把它切成多长一段"。
const DURATIONS = [30, 60, 90, 120, 180]

const chapters = computed(() => story.value?.chapters ?? [])
const plan = computed(() => story.value?.plan ?? [])
const durationS = computed(() => story.value?.episode_duration_s ?? 60)
const hasStory = computed(() => chapters.value.length > 0)
const writtenCount = computed(
  () => chapters.value.filter((c) => (c.text ?? '').trim()).length,
)
/** 停在有说法的钩子上的集数。剩下的收在段落边界——到点了，不是悬念。 */
const hooked = computed(() => plan.value.filter((p) => p.hook).length)

/** 这一章之后要画的那几条分集线（在这一章结束的集）。 */
function cutsAfter(chapterId) {
  return plan.value.filter((p) => p.to_chapter === chapterId)
}

// ---------------------------------------------------------------------------
// 每一集用到的人和地方
// ---------------------------------------------------------------------------
//
// 用户 2026-09-12：「分集里每集用到的角色和场景图应该显示出来，容易区分」。
//
// **分集表最难的就是分不清。** 十来行「第 N 集 · 60s · 某某悬念」，字都
// 差不多长、颜色都一样，要找"陈默在医院那一集"只能一行行读过去。而这几集
// 之间真正的差别是**谁在场、在哪儿**——那正好是图。
//
// 名单从章节来（`chapter.characters` / `chapter.locations`，是「读故事」
// 那一步照着正文读出来的），图从资产库来。两边靠**名字**对上：章节里存的
// 就是名字，不是 id。

/** 名字 → 角色。资产库里存的是 char_id，而章节里记的是名字。 */
const charByName = computed(() => {
  const m = new Map()
  for (const c of assets.value?.characters ?? []) m.set(c.name, c)
  return m
})
const locByName = computed(() => {
  const m = new Map()
  for (const l of assets.value?.locations ?? []) m.set(l.name, l)
  return m
})

/** 这一集覆盖的那几章。分集表里存的是首尾章号，中间的按顺序取。 */
function chaptersOf(ep) {
  const from = chapters.value.findIndex((c) => c.chapter_id === ep.from_chapter)
  const to = chapters.value.findIndex((c) => c.chapter_id === ep.to_chapter)
  if (from < 0 || to < 0) return []
  return chapters.value.slice(from, to + 1)
}

/**
 * 这一集自己那段正文。
 *
 * **一集常常只是一章的一截**：一章三千字切成三集，三集共用一份章级名单的
 * 话，那三行看着一模一样——而这一栏存在的全部理由就是让人分得清。
 *
 * `from_char` / `to_char` 是**各自那一章里**的偏移（首章从 from_char 到尾，
 * 末章从头到 to_char，中间整章）。它们是码点偏移，而 JS 的 slice 按 UTF-16
 * 数——中文都在基本平面上，两者一致；真混进 emoji 也只是偏几个字，
 * 对"名字在不在这一段里"没有影响。
 */
function sliceOf(ep) {
  const list = chaptersOf(ep)
  if (!list.length) return ''
  let out = ''
  list.forEach((c, i) => {
    const text = c.text ?? ''
    const a = i === 0 ? (ep.from_char ?? 0) : 0
    const b = i === list.length - 1 ? (ep.to_char ?? text.length) : text.length
    out += text.slice(a, b)
  })
  return out
}

/**
 * 这一集有谁、在哪儿。
 *
 * 两步：**名单从章节来，在不在场看这一集自己那段正文**。
 *
 *   * 候选名单是 `chapter.characters` / `chapter.locations`——「读故事」那一步
 *     照着正文读出来的，是权威的那一份。
 *   * **老项目里这两项是空的**：2026-09-12 之前 schema 没把它们写进
 *     required，14B 就一个都不给（见 story_outline.cpp 里那段）。那时候候选
 *     退成资产库里登记过的全部名字。
 *   * 然后拿这一集的正文过一遍：出现过的才算在场。人名在正文里是实打实
 *     写出来的，扫得准；地名多半扫不到（正文里很少原样写"高架桥下的咖啡
 *     馆"），扫不到就空着，不猜。
 *
 * 正文还没写的章走不到第二步（没得扫），那就直接用章级名单——那时候它是
 * 计划，显示计划是对的。
 */
function castOf(ep, key, listKey, lookup, refKey) {
  const covered = chaptersOf(ep)
  const listed = []
  for (const c of covered) {
    for (const n of c[listKey] ?? []) if (!listed.includes(n)) listed.push(n)
  }
  const candidates = listed.length ? listed : [...lookup.keys()]

  const text = sliceOf(ep)
  let names = text ? candidates.filter((n) => n && text.includes(n)) : []
  // 扫不出来（正文没写，或者名字确实没在这一段里出现）就退回章级名单。
  // **空着比错着好，但全空就等于这一栏不存在**——所以只在完全扫不到时退。
  if (!names.length) names = listed

  return names.map((name) => {
    const hit = lookup.get(name)
    return {
      name,
      // 没有图就只给名字，界面上退成一个字的小牌子——**比不显示强**：
      // 这一栏存在的理由就是让人一眼分清哪一集是哪一集，而名字也分得清。
      url: hit?.[refKey] ? mediaUrl(session.projectPath, hit[refKey]) : '',
    }
  })
}

const facesOf = (ep) =>
  castOf(ep, 'who', 'characters', charByName.value, 'ref_front')
const scenesOf = (ep) =>
  castOf(ep, 'where', 'locations', locByName.value, 'ref_empty')

async function load() {
  if (!session.projectPath) {
    story.value = null
    return
  }
  loading.value = true
  try {
    // 两份一起拉：分集线上要显示的人脸和空景图在资产库里，
    // 而一集是哪几个人在哪几个地方，在故事里。
    const [got, lib] = await Promise.all([
      api.getStory(session.projectPath),
      // 资产库拉不动不该把整页挡住——那时候分集线退成只有名字。
      api.assets(session.projectPath).catch(() => null),
    ])
    story.value = got.story ?? null
    assets.value = lib
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
  }
}
watch(() => session.projectPath, load, { immediate: true })

async function pickDuration(event) {
  const seconds = Number(event.target.value)
  // **改时长就是重新分集。** 存一个数然后等人再按一次「重算」，那一下
  // 之间界面上写的集数是旧的，而用户以为已经改了。
  const result = await run(
    () =>
      api.planEpisodes({ project: session.projectPath, duration_s: seconds }),
    { key: 'duration' },
  )
  if (!result) return
  story.value = result.story ?? story.value
  ui.ok(`每集 ${seconds} 秒 → ${plan.value.length} 集`)
}

async function makeEpisodes() {
  const result = await run(
    () => api.makeEpisodes({ project: session.projectPath }),
    { key: 'episodes', refresh: true },
  )
  if (!result) return
  const created = result.created?.length ?? 0
  const updated = result.updated?.length ?? 0
  ui.ok(created ? `建了 ${created} 集` : `${updated} 集已经在了，只更新了信息`)
  await load()
}

// ---------------------------------------------------------------------------
// 支线。整部剧只用一两次的东西，收在折叠区里
// ---------------------------------------------------------------------------

const extras = ref(false)
const trailerDraft = ref(null)
const trailerDurationS = ref(20)
/** 预告片挂在固定集号上，只有一条，重剪覆盖上一条 */
const TRAILER_ID = 'trailer'

/**
 * 剪一条预告片。
 *
 * 对流水线来说预告片就是特别短的一集：采用之后照样走镜头、成片、发布。
 * 区别只在写的时候——要的是钩子不是完整故事，所以它**不占集号**，也不参与
 * 「接着前几集写」的上下文。
 */
async function writeTrailer() {
  const result = await run(
    () =>
      api.writeTrailer({
        project: session.projectPath,
        duration_s: trailerDurationS.value,
      }),
    { key: 'trailer' },
  )
  if (result) trailerDraft.value = result
}

async function adoptTrailer() {
  if (!trailerDraft.value) return
  await run(
    async () => {
      const exists = session.episodes.some((e) => e.episode_id === TRAILER_ID)
      if (!exists) {
        await api.newEpisode({
          project: session.projectPath,
          episode_id: TRAILER_ID,
          title: trailerDraft.value.title,
          target_duration_s: trailerDurationS.value,
        })
      }
      await api.saveScript({
        project: session.projectPath,
        episode_id: TRAILER_ID,
        script: trailerDraft.value.script,
        duration_s: trailerDurationS.value,
        synopsis: trailerDraft.value.logline,
      })
      trailerDraft.value = null
    },
    { key: 'adoptTrailer', success: '预告片存下了', refresh: true },
  )
}

/** 手动加一集。没走故事那条路的老项目还得有这个口子。 */
async function addEpisode() {
  const created = await run(
    () =>
      api.newEpisode({
        project: session.projectPath,
        target_duration_s: durationS.value,
      }),
    { key: 'addEp', success: '新建了一集' },
  )
  if (created) {
    await session.refresh()
    session.selectEpisode(created.episode_id)
  }
}

defineExpose({ load })
</script>

<template>
  <div class="eps">
    <!-- 同一个故事，每集多长决定切成几集。集数是算出来的。改时长就是重新分集。 -->
    <div class="toolbar">
      <label class="dur">
        <span class="tiny dim">每集</span>
        <select
          class="select dur__pick"
          :value="durationS"
          :disabled="isBusy('duration')"
          title="改时长就是重新分集"
          @change="pickDuration"
        >
          <option v-for="d in DURATIONS" :key="d" :value="d">{{ d }} 秒</option>
        </select>
      </label>
      <span v-if="hasStory" class="tiny dim nowrap">
        {{ chapters.length }} 章 → {{ plan.length }} 集<template v-if="plan.length">
          · {{ hooked }} 集停在悬念上</template>
        <template v-if="writtenCount < chapters.length">
          · {{ chapters.length - writtenCount }} 章还没正文</template>
      </span>
      <span class="spacer" />
      <button
        v-if="hasStory"
        class="btn btn--primary btn--sm"
        type="button"
        :disabled="isBusy('episodes')"
        title="分集表是计划，落成剧集之后后面几步才有东西可对"
        @click="makeEpisodes"
      >
        {{ isBusy('episodes') ? '正在建…' : '落成剧集' }}
      </button>
    </div>

    <div v-if="loading" class="tiny dim">读取中…</div>

    <EmptyState v-else-if="!hasStory" icon="book" title="还没有故事">
      <RouterLink to="/story" class="btn btn--sm">去写故事</RouterLink>
    </EmptyState>

    <!-- 章节一行，分集线画在两行之间。线在哪一眼就看得见。 -->
    <section v-else class="stack stack--sm">
      <template v-for="(c, i) in chapters" :key="c.chapter_id">
        <div
          class="chap"
          :class="{ 'is-open': openChapter === c.chapter_id }"
          @click="openChapter = openChapter === c.chapter_id ? '' : c.chapter_id"
        >
          <span class="chap__no numeric">{{ i + 1 }}</span>
          <div class="chap__text">
            <div class="chap__title">{{ c.title }}</div>
            <p v-if="openChapter === c.chapter_id && c.summary" class="chap__sum">
              {{ c.summary }}
            </p>
          </div>
          <span v-if="c.text" class="tiny dim numeric nowrap">
            {{ [...c.text].length }} 字
          </span>
          <RouterLink v-else to="/story" class="btn btn--sm btn--ghost nowrap" @click.stop>
            去展开正文
          </RouterLink>
        </div>

        <div v-for="ep in cutsAfter(c.chapter_id)" :key="ep.episode_id" class="cut">
          <span class="cut__id numeric">{{ ep.episode_id }}</span>
          <span class="cut__dur numeric">{{ ep.target_duration_s }}s</span>

          <!-- 这一集里谁在场、在哪儿。**人是圆的，地方是方的**——形状不一样，
               扫一眼就分得开，不用去读底下那行字。 -->
          <span class="cast">
            <span
              v-for="f in facesOf(ep)"
              :key="'c' + f.name"
              class="cast__one cast__one--who"
              :title="f.name"
            >
              <img v-if="f.url" :src="f.url" :alt="f.name" loading="lazy" />
              <i v-else>{{ [...f.name][0] }}</i>
            </span>
            <span
              v-for="l in scenesOf(ep)"
              :key="'l' + l.name"
              class="cast__one cast__one--where"
              :title="l.name"
            >
              <img v-if="l.url" :src="l.url" :alt="l.name" loading="lazy" />
              <i v-else>{{ [...l.name][0] }}</i>
            </span>
          </span>

          <span v-if="ep.hook" class="cut__hook truncate">{{ ep.hook }}</span>
          <span v-else class="cut__hook dim">章尾</span>
        </div>
      </template>
    </section>

    <!-- 支线。整部剧只用一两次的东西，收在折叠区里 -->
    <section class="stack stack--sm">
      <button class="fold" type="button" @click="extras = !extras">
        <AppIcon :name="extras ? 'arrowLeft' : 'arrowRight'" :size="14" />
        <span>预告片 · 手动加一集</span>
      </button>

      <div v-if="extras" class="stack stack--sm">
        <section v-if="trailerDraft" class="draft">
          <div class="sec__head">
            <h2 class="sec__t">{{ trailerDraft.title }}</h2>
            <span class="tiny dim truncate">{{ trailerDraft.logline }}</span>
            <span class="spacer" />
            <button
              class="btn btn--primary btn--sm"
              type="button"
              :disabled="isBusy('adoptTrailer')"
              @click="adoptTrailer"
            >
              存成 trailer 这一集
            </button>
            <button
              class="btn btn--ghost btn--sm"
              type="button"
              @click="trailerDraft = null"
            >
              丢弃
            </button>
          </div>
          <pre class="mono small trailer__script">{{ trailerDraft.script }}</pre>
        </section>

        <div class="row row--wrap">
          <label class="dur">
            <span class="tiny dim">预告片</span>
            <select v-model.number="trailerDurationS" class="select dur__pick">
              <option :value="15">15 秒</option>
              <option :value="20">20 秒</option>
              <option :value="30">30 秒</option>
            </select>
          </label>
          <button
            class="btn btn--ai btn--sm"
            type="button"
            :disabled="!hasStory || isBusy('trailer')"
            @click="writeTrailer"
          >
            {{ isBusy('trailer') ? '剪着…' : '剪一条' }}
          </button>
          <span class="spacer" />
          <button
            class="btn btn--ghost btn--sm"
            type="button"
            :disabled="isBusy('addEp')"
            title="加出来的那集不在分集表里，走老路径"
            @click="addEpisode"
          >
            手动加一集
          </button>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.eps {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
}
.dur {
  display: inline-flex;
  align-items: center;
  gap: var(--s2);
}
.dur__pick {
  width: auto;
  height: 27px;
  padding: 0 var(--s2);
}
.draft {
  border: 1px dashed var(--accent-line);
  border-radius: var(--r);
  padding: var(--s3);
}

/* 章节一行，分集线画在两行之间——线在哪一眼就看得见，
   这正是不把分集做成另一张表的理由 */
.chap {
  display: flex;
  align-items: baseline;
  gap: 10px;
  padding: 8px 12px;
  border: 1px solid var(--line);
  border-radius: var(--r-sm);
  background: var(--surface);
  cursor: pointer;
}
.chap.is-open {
  border-color: var(--accent);
}
.chap__no {
  color: var(--text-3);
  font-size: var(--fs-sm);
  min-width: 1.5em;
}
.chap__text {
  flex: 1;
  min-width: 0;
}
.chap__title {
  font-weight: 500;
}
.chap__sum {
  margin: 4px 0 0;
  font-size: var(--fs-sm);
  color: var(--text-2);
}

.cut {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 2px 12px;
  font-size: var(--fs-xs);
  color: var(--accent);
  border-top: 1px dashed var(--accent);
  margin: 2px 0;
}
.cut__id {
  font-weight: 600;
}
.cut__dur {
  color: var(--text-3);
}
.cut__hook {
  flex: 1;
  min-width: 0;
}

/* 这一集用到的人和地方。**挤在分集线上，不另起一行**——它是用来区分
   相邻几集的，离开那条线就失去了参照。 */
.cast {
  display: flex;
  align-items: center;
  gap: 3px;
  flex: none;
}
.cast__one {
  display: grid;
  place-items: center;
  width: 22px;
  height: 22px;
  overflow: hidden;
  background: var(--surface-3);
  color: var(--text-3);
  font-size: 10px;
  font-style: normal;
  line-height: 1;
}
.cast__one img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
/* 人是圆的。头像裁成圆的时候脸在正中间，而参考图是正面全身——
   所以取上面那一截。 */
.cast__one--who {
  border-radius: 50%;
}
.cast__one--who img {
  object-position: top center;
}
/* 地方是方的（带一点圆角），而且宽一些：空景图是 16:9，裁成正方形
   基本只剩中间一堵墙。 */
.cast__one--where {
  width: 34px;
  border-radius: var(--r-sm);
}

.fold {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: 6px 0;
  border: 0;
  background: transparent;
  color: var(--text-2);
  cursor: pointer;
}
.trailer__script {
  white-space: pre-wrap;
  max-height: 20em;
  overflow: auto;
  margin: 0;
}
</style>
