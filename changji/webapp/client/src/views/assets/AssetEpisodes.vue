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
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

const story = ref(null)
const loading = ref(false)
const openChapter = ref('')

const SCALES = [
  { key: 'short', label: '短篇', hint: '四章左右，一口气讲完' },
  { key: 'medium', label: '中篇', hint: '八章左右' },
  { key: 'long', label: '长篇', hint: '十六章左右，主线能铺开' },
]
const DURATIONS = [30, 60, 90, 120, 180]

const chapters = computed(() => story.value?.chapters ?? [])
const plan = computed(() => story.value?.plan ?? [])
const scale = computed(() => story.value?.scale ?? 'medium')
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

async function load() {
  if (!session.projectPath) {
    story.value = null
    return
  }
  loading.value = true
  try {
    story.value = (await api.getStory(session.projectPath)).story ?? null
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
  }
}
watch(() => session.projectPath, load, { immediate: true })

async function pickScale(key) {
  const result = await run(
    () => api.saveStory({ project: session.projectPath, scale: key }),
    { key: 'scale' },
  )
  if (result) story.value = result.story ?? story.value
}

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

defineExpose({ load })
</script>

<template>
  <div class="stack stack--lg">
    <h2 class="asec">
      分集
      <span class="asec__sub">
        同一个故事，每集 30 秒切出来十几集，每集 3 分钟切出来三四集。
        集数是算出来的，不用填。
      </span>
    </h2>

    <section class="card">
      <div class="card__body row row--wrap">
        <div class="scales">
          <button
            v-for="s in SCALES"
            :key="s.key"
            class="scale"
            :class="{ 'is-on': scale === s.key }"
            type="button"
            :title="s.hint"
            :disabled="isBusy('scale')"
            @click="pickScale(s.key)"
          >
            {{ s.label }}
          </button>
        </div>

        <label class="field field--inline">
          <span class="field__label">每集</span>
          <select
            class="select select--slim"
            :value="durationS"
            :disabled="isBusy('duration')"
            @change="pickDuration"
          >
            <option v-for="d in DURATIONS" :key="d" :value="d">{{ d }} 秒</option>
          </select>
        </label>

        <span v-if="hasStory" class="pill pill--accent nowrap">
          {{ chapters.length }} 章 → {{ plan.length }} 集
        </span>
        <span v-if="hasStory && plan.length" class="tiny dim nowrap">
          {{ hooked }} 集停在真悬念上
        </span>
        <span class="spacer" />
        <button
          v-if="hasStory"
          class="btn btn--primary btn--sm"
          type="button"
          :disabled="isBusy('episodes')"
          @click="makeEpisodes"
        >
          {{ isBusy('episodes') ? '正在建…' : '落成剧集' }}
        </button>
      </div>
    </section>

    <div v-if="loading" class="tiny dim">读取中…</div>

    <EmptyState
      v-else-if="!hasStory"
      icon="book"
      title="还没有故事"
      hint="先去「故事」那一页写出来，这里才有东西可切。"
    >
      <RouterLink to="/story" class="btn">去写故事</RouterLink>
    </EmptyState>

    <section v-else class="stack stack--sm">
      <div class="row row--between">
        <span class="tiny dim">
          章节 {{ chapters.length }} · 已展开正文 {{ writtenCount }}
          <template v-if="writtenCount < chapters.length">
            （没展开的那些，写剧本时用的是梗概不是正文）
          </template>
        </span>
        <span class="tiny dim">横线就是分集，切在钩子上</span>
      </div>

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
          <span v-if="ep.hook" class="cut__hook truncate">钩子：{{ ep.hook }}</span>
          <span v-else class="cut__hook dim">章尾</span>
        </div>
      </template>

      <p class="tiny dim">
        分集表是计划。落成剧集之后，后面几步才有东西可对。
        <AppIcon name="info" :size="13" />
      </p>
    </section>
  </div>
</template>

<style scoped>
.asec {
  font-size: var(--fs-lg);
  margin: 0;
}
.asec__sub {
  display: block;
  font-size: var(--fs-sm);
  font-weight: 400;
  color: var(--text-3);
  margin-top: 2px;
}

.scales {
  display: inline-flex;
  border: 1px solid var(--line);
  border-radius: var(--r-sm);
  overflow: hidden;
}
.scale {
  padding: 5px 12px;
  border: 0;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.scale.is-on {
  background: var(--accent-soft);
  color: var(--accent);
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
</style>
