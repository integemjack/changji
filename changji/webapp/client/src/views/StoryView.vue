<script setup>
/**
 * 故事。整条流水线的源头。
 *
 * 这一页回答的是「这部剧讲什么、分几集」。**集数不在这里填**——它是按
 * 故事体量和每集时长算出来的。原来那个「我要写 N 集」的输入框没有了，
 * 那正是逐集续写那套的病根：集数由人拍脑袋定，故事就没有全局结构、
 * 写到第五集开始失忆、也永远没有结尾。
 *
 * 分集是**章节之间那条线**，不是另一张表。单独做成一页的话，人只能看到
 * 「第 3 集覆盖第 5~6 章」而看不见线画在哪，还得回去翻第 5 章是什么。
 *
 * 右栏是**索引不是画廊**：名字 + 一句话，点开去角色页。人物和场景本来
 * 就是从故事里提的，摆在故事旁边才看得出这层关系。往右栏里塞可编辑的
 * 字段，这一页就会长回 ScriptView 今天的样子。
 */
import { computed, onMounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import StepHeader from '@/components/StepHeader.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

const story = ref(null)
const loading = ref(false)
const draft = ref(null) // AI 写完、还没采用的那一份
const keywords = ref('')
const openChapter = ref('')
const pasting = ref(false)
const pasted = ref('')

const premise = ref('')
const savedPremise = ref('')

const SCALES = [
  { key: 'short', label: '短篇', hint: '四章左右，一口气讲完' },
  { key: 'medium', label: '中篇', hint: '八章左右' },
  { key: 'long', label: '长篇', hint: '十六章左右，主线能铺开' },
]
const DURATIONS = [30, 60, 90, 120, 180]

const chapters = computed(() => story.value?.chapters ?? [])
const plan = computed(() => story.value?.plan ?? [])
const characters = computed(() => story.value?.characters ?? [])
const locations = computed(() => story.value?.locations ?? [])
const relations = computed(() => story.value?.relations ?? [])
const scale = computed(() => story.value?.scale ?? 'medium')
const durationS = computed(() => story.value?.episode_duration_s ?? 60)
const hasStory = computed(() => chapters.value.length > 0)
const premiseDirty = computed(() => premise.value.trim() !== savedPremise.value)
/** 有正文但一个人物都没提出来——粘进来的故事就是这样。 */
const needsAnalysis = computed(
  () => hasStory.value && !characters.value.length && writtenCount.value > 0,
)
const writtenCount = computed(
  () => chapters.value.filter((c) => (c.text ?? '').trim()).length,
)

/** 这一章之后要画的那几条分集线（在这一章结束的集）。 */
function cutsAfter(chapterId) {
  return plan.value.filter((p) => p.to_chapter === chapterId)
}

function relationsOf(name) {
  return relations.value.filter((r) => r.a === name || r.b === name)
}

function setStory(payload) {
  story.value = payload?.story ?? null
  premise.value = story.value?.premise ?? ''
  savedPremise.value = premise.value.trim()
}

async function load() {
  if (!session.projectPath) {
    story.value = null
    return
  }
  loading.value = true
  try {
    setStory(await api.getStory(session.projectPath))
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
  }
}

onMounted(load)
watch(() => session.projectPath, load)

async function savePremise() {
  if (!premiseDirty.value || !session.projectPath) return
  const result = await run(
    () =>
      api.saveStory({ project: session.projectPath, premise: premise.value.trim() }),
    { key: 'premise', success: '梗概已存下' },
  )
  if (result) setStory(result)
}

async function pickScale(next) {
  if (next === scale.value) return
  const result = await run(
    () => api.saveStory({ project: session.projectPath, scale: next }),
    { key: 'scale' },
  )
  if (result) setStory(result)
}

/**
 * 换每集时长。
 *
 * 有故事就重算分集表——集数跟着时长走，这是这一页的主张。没故事就只存
 * 下来，等写完大纲再算。
 */
async function pickDuration(event) {
  const next = Number(event.target.value)
  const result = await run(
    () =>
      hasStory.value
        ? api.planEpisodes({ project: session.projectPath, duration_s: next })
        : api.saveStory({
            project: session.projectPath,
            episode_duration_s: next,
          }),
    { key: 'duration' },
  )
  if (result) setStory(result)
}

/**
 * 写故事。
 *
 * **梗概不是必填的。** 三个入口里只有「我自己有个想法」那条是从手写的
 * 一句话开始的；给几个关键词、或者什么都不给让它来一个，同样正当。
 * 选题本来就是整条流水线上最难从零开始的一步，把它做成硬门槛等于又把人
 * 摁回空白框前面发呆。空着写出来的那一句会回填到梗概框里。
 */
async function writeStory() {
  const result = await run(
    () =>
      api.writeOutline({
        project: session.projectPath,
        premise: premise.value.trim(),
        scale: scale.value,
        keywords: keywords.value.trim(),
      }),
    { key: 'write' },
  )
  if (result) draft.value = result
}

/**
 * 把分集表落成真的剧集。
 *
 * 和「采用大纲」分开，是因为改每集时长是个随手的动作（当场重算分集表），
 * 而建剧集会动到已经写好剧本、已经出过片的那几集。后端对已存在的同号
 * 剧集只补元数据，script 和 shots 一个字不碰。
 */
async function makeEpisodes() {
  const result = await run(
    () => api.makeEpisodes({ project: session.projectPath }),
    { key: 'episodes', refresh: true },
  )
  if (result) {
    setStory(result)
    const n = result.created?.length ?? 0
    ui.ok(n ? `建了 ${n} 集，后面几步可以对着它们干活了` : '剧集都在，元数据对齐了一遍')
  }
}

/**
 * 粘一段现成的东西进来。
 *
 * 三个入口里的第二条。切章节不走大模型——那是机械活，靠标题行和段落边界
 * 就能做，而且比模型稳：同一段文本让模型切两次，结果会不一样。
 *
 * 切出来的章节自带正文，所以后面写剧本时展开的是真正文，不是梗概。
 */
async function importPasted() {
  if (!pasted.value.trim()) {
    ui.warn('先把文本粘进来')
    return
  }
  const result = await run(
    () => api.importStory({ project: session.projectPath, text: pasted.value }),
    { key: 'import' },
  )
  if (result) {
    draft.value = result
    pasting.value = false
    pasted.value = ''
  }
}

/**
 * 让 AI 读一遍正文。
 *
 * 粘进来的故事只有正文，人物关系地点全是空的——走到「设定」那一步资产库
 * 还是空的，再往下分镜指不到任何角色。这一步补那个洞，**正文一个字不动**。
 *
 * 顺带把机械切点换成真钩子：粘贴时登记的段落边界只保证不切在半句话中间，
 * 读过之后能切在真正的悬念上，所以分集表会跟着重算。
 */
async function analyzeStory() {
  const result = await run(
    () => api.analyzeStory({ project: session.projectPath }),
    { key: 'analyze' },
  )
  if (result) draft.value = result
}

async function adoptDraft() {
  if (!draft.value) return
  const result = await run(
    () =>
      api.adoptStory({
        project: session.projectPath,
        story: draft.value.story,
        // 已经展开过正文时后端会拦一下，这里明确表示"就是要换"
        overwrite: true,
      }),
    { key: 'adopt', success: '采用了，写进项目了', refresh: true },
  )
  if (result) {
    setStory(result)
    draft.value = null
  }
}
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader>
      <template #actions>
        <button
          class="btn btn--ai"
          type="button"
          :disabled="!session.hasProject || isBusy('write')"
          @click="writeStory"
        >
          <AppIcon name="sparkle" :size="15" />
          {{
            isBusy('write')
              ? '大模型正在写…'
              : hasStory
                ? '重写故事'
                : premise.trim() || keywords.trim()
                  ? 'AI 写故事'
                  : 'AI 来一个'
          }}
        </button>
      </template>
    </StepHeader>

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="故事挂在项目上。先回第一步选一个，或者新建一个。"
    >
      <RouterLink to="/project" class="btn btn--primary">去第一步</RouterLink>
    </EmptyState>

    <template v-else>
      <!-- 草稿。AI 写完先摆出来给人看，点了采用才落库 -->
      <section v-if="draft" class="card card--draft">
        <div class="card__head">
          <div>
            <div class="card__title">
              <AppIcon name="sparkle" :size="15" class="inline-icon" />
              大模型写了一份，还没存
            </div>
            <div class="card__sub">{{ draft.story?.logline }}</div>
            <div
              v-if="draft.story?.premise && draft.story.premise !== savedPremise"
              class="tiny dim"
            >
              选题：{{ draft.story.premise }}
            </div>
          </div>
          <span class="pill pill--accent nowrap">
            {{ draft.chapters }} 章 · 分 {{ draft.episodes }} 集
          </span>
        </div>
        <div v-if="draft.needs_analysis" class="card__body">
          <p class="tiny dim">
            粘进来的只有正文，人物和地点还没提出来——那要读懂内容才做得到，
            是下一步的事。先采用，章节和分集已经能看了。
          </p>
        </div>
        <div class="card__body stack stack--sm">
          <ol class="draftlist">
            <li v-for="c in draft.story?.chapters ?? []" :key="c.chapter_id">
              <b>{{ c.title }}</b>
              <span class="dim">{{ c.summary }}</span>
            </li>
          </ol>
          <div class="row">
            <button
              class="btn btn--primary"
              type="button"
              :disabled="isBusy('adopt')"
              @click="adoptDraft"
            >
              采用这一份
            </button>
            <button class="btn btn--ghost" type="button" @click="draft = null">
              丢弃
            </button>
          </div>
        </div>
      </section>

      <div class="story">
        <div class="story__main stack">
          <!-- 梗概 + 体量 + 每集时长。集数是算出来的，不在这里填 -->
          <section class="card">
            <div class="card__body stack">
              <textarea
                v-model="premise"
                class="textarea"
                rows="3"
                placeholder="想好了就写一句，比如：深夜便利店，前任推门进来，手里拿着五年前她送的那把伞。&#10;没想好就空着，直接点右上角让它来一个。"
                @blur="savePremise"
              />

              <div class="row row--wrap">
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
                    <option v-for="d in DURATIONS" :key="d" :value="d">
                      {{ d }} 秒
                    </option>
                  </select>
                </label>

                <span v-if="hasStory" class="pill pill--accent nowrap">
                  {{ chapters.length }} 章 → {{ plan.length }} 集
                </span>
                <span v-else class="tiny dim">
                  集数由故事体量和每集时长算出来，不用填
                </span>
              </div>

              <input
                v-model="keywords"
                class="input"
                placeholder="想往哪个方向？热点词、题材都行，可留空（比如：重生复仇、破镜重圆）"
              />
            </div>
          </section>

          <!-- 章节 + 分集切线 -->
          <section class="stack stack--sm">
            <div v-if="loading" class="tiny dim">读取中…</div>

            <section v-if="pasting" class="card">
              <div class="card__head">
                <div>
                  <div class="card__title">粘一段现成的进来</div>
                  <div class="card__sub">
                    小说、剧本、大纲都行。认得出「第三章」「## 标题」就照它分章，
                    认不出就按字数在段落边界上切。版权自负。
                  </div>
                </div>
                <button
                  class="btn btn--ghost btn--sm"
                  type="button"
                  @click="pasting = false"
                >
                  收起
                </button>
              </div>
              <div class="card__body stack stack--sm">
                <textarea
                  v-model="pasted"
                  class="textarea"
                  rows="10"
                  placeholder="把正文粘在这里…"
                />
                <div class="row row--between">
                  <span class="tiny dim numeric">
                    {{ [...pasted].length }} 字
                  </span>
                  <button
                    class="btn btn--primary"
                    type="button"
                    :disabled="isBusy('import')"
                    @click="importPasted"
                  >
                    {{ isBusy('import') ? '切分中…' : '切成章节' }}
                  </button>
                </div>
              </div>
            </section>

            <EmptyState
              v-else-if="!hasStory"
              icon="script"
              title="还没有故事"
              hint="选个体量，点右上角让它写。梗概和方向都可以空着——空着就让它自己定选题。"
            >
              <button class="btn" type="button" @click="pasting = true">
                我有现成的，粘进来
              </button>
            </EmptyState>

            <template v-else>
              <div class="row row--between">
                <span class="tiny dim">
                  章节 {{ chapters.length }}
                  <template v-if="writtenCount">
                    · 已展开正文 {{ writtenCount }}
                  </template>
                </span>
                <div class="row">
                  <button
                    v-if="needsAnalysis"
                    class="btn btn--ai btn--sm"
                    type="button"
                    :disabled="isBusy('analyze')"
                    @click="analyzeStory"
                  >
                    {{ isBusy('analyze') ? '正在读…' : '让 AI 读一遍，提人物' }}
                  </button>
                  <button
                    class="btn btn--ghost btn--sm"
                    type="button"
                    @click="pasting = !pasting"
                  >
                    粘一段进来
                  </button>
                  <span class="tiny dim">横线就是分集，切在钩子上</span>
                </div>
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
                    <div v-if="openChapter === c.chapter_id" class="chap__sum">
                      {{ c.summary }}
                    </div>
                  </div>
                  <span v-if="c.text" class="tiny dim numeric nowrap">
                    {{ [...c.text].length }} 字
                  </span>
                  <span v-else class="pill pill--neutral tiny nowrap">只有大纲</span>
                </div>

                <div v-for="ep in cutsAfter(c.chapter_id)" :key="ep.episode_id" class="cut">
                  <span class="cut__id numeric">{{ ep.episode_id }}</span>
                  <span class="cut__dur numeric">{{ ep.target_duration_s }}s</span>
                  <span v-if="ep.hook" class="cut__hook truncate">
                    钩子：{{ ep.hook }}
                  </span>
                  <span v-else class="cut__hook dim">章尾</span>
                </div>
              </template>

              <div class="row row--between">
                <span class="tiny dim">
                  分集表是计划。落成剧集之后，后面几步才有东西可对。
                </span>
                <button
                  class="btn btn--primary btn--sm"
                  type="button"
                  :disabled="isBusy('episodes')"
                  @click="makeEpisodes"
                >
                  {{ isBusy('episodes') ? '正在建…' : '落成剧集' }}
                </button>
              </div>
            </template>
          </section>
        </div>

        <!-- 右栏：索引，不是画廊。点开去角色页改外观 -->
        <aside v-if="hasStory" class="story__side stack stack--sm">
          <div v-if="needsAnalysis" class="side__group side__group--warn">
            <div class="side__head">还没提人物</div>
            <p class="side__hint tiny">
              粘进来的只有正文。让 AI 读一遍才能把人和地方提出来，
              不然走到「设定」那一步资产库是空的。
            </p>
          </div>

          <div class="side__group">
            <div class="side__head">人 {{ characters.length }}</div>
            <RouterLink
              v-for="c in characters"
              :key="c.name"
              to="/characters"
              class="side__row"
              :title="c.identity"
            >
              <span class="side__name truncate">{{ c.name }}</span>
              <span v-if="relationsOf(c.name).length" class="tiny dim nowrap">
                {{ relationsOf(c.name).length }} 段关系
              </span>
            </RouterLink>
          </div>

          <div class="side__group">
            <div class="side__head">地方 {{ locations.length }}</div>
            <RouterLink
              v-for="l in locations"
              :key="l.name"
              to="/scenes"
              class="side__row"
              :title="l.what"
            >
              <span class="side__name truncate">{{ l.name }}</span>
            </RouterLink>
          </div>

          <div v-if="relations.length" class="side__group">
            <div class="side__head">关系</div>
            <div v-for="(r, i) in relations" :key="i" class="side__rel tiny">
              <b>{{ r.a }} — {{ r.b }}</b>
              <span class="dim">{{ r.kind }}</span>
            </div>
          </div>
        </aside>
      </div>
    </template>
  </div>
</template>

<style scoped>
.story {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 15rem;
  gap: var(--s5);
  align-items: start;
}
@media (max-width: 900px) {
  .story {
    grid-template-columns: minmax(0, 1fr);
  }
}
.story__main {
  min-width: 0;
}

/* ---- 体量三档 ---- */

.scales {
  display: flex;
  gap: 2px;
  padding: 2px;
  background: var(--surface-2);
  border: 1px solid var(--line);
  border-radius: 10px;
}
.scale {
  padding: 5px 12px;
  border: 0;
  border-radius: 8px;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.scale.is-on {
  background: var(--surface);
  color: var(--accent);
  font-weight: 600;
  box-shadow: 0 1px 2px rgb(0 0 0 / 8%);
}

/* ---- 章节与分集切线 ---- */

.chap {
  display: flex;
  align-items: center;
  gap: var(--s3);
  padding: var(--s3) var(--s4);
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: 10px;
  cursor: pointer;
}
.chap:hover {
  border-color: var(--accent-line);
}
.chap__no {
  flex: none;
  width: 1.6rem;
  color: var(--text-3);
  font-size: var(--fs-sm);
}
.chap__text {
  flex: 1;
  min-width: 0;
}
.chap__title {
  font-weight: 600;
}
.chap__sum {
  margin-top: 2px;
  color: var(--text-2);
  font-size: var(--fs-sm);
  line-height: 1.6;
}

/* 分集就是章节之间这条线。它是这一页的主角，所以给足对比度。 */
.cut {
  display: flex;
  align-items: center;
  gap: var(--s3);
  margin: 2px 0;
  padding: 3px var(--s4);
  border-top: 2px dashed var(--accent-line);
  color: var(--accent);
  font-size: var(--fs-sm);
}
.cut__id {
  font-weight: 700;
}
.cut__dur {
  color: var(--text-3);
}
.cut__hook {
  min-width: 0;
  color: var(--text-2);
}

/* ---- 右栏索引 ---- */

.story__side {
  position: sticky;
  top: var(--s4);
}
.side__group {
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: 10px;
  overflow: hidden;
}
.side__head {
  padding: var(--s2) var(--s3);
  background: var(--surface-2);
  border-bottom: 1px solid var(--line);
  color: var(--text-3);
  font-size: var(--fs-xs);
  font-weight: 600;
}
.side__row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--s2);
  padding: var(--s2) var(--s3);
  color: var(--text);
  text-decoration: none;
  font-size: var(--fs-sm);
}
.side__row:hover {
  background: var(--accent-soft);
  text-decoration: none;
}
.side__name {
  min-width: 0;
}
.side__group--warn {
  border-color: color-mix(in srgb, var(--warn) 40%, transparent);
}
.side__hint {
  padding: var(--s2) var(--s3);
  color: var(--text-2);
  line-height: 1.6;
}
.side__rel {
  display: flex;
  justify-content: space-between;
  gap: var(--s2);
  padding: 4px var(--s3);
}

/* ---- 草稿 ---- */

.draftlist {
  margin: 0;
  padding-left: 1.4rem;
  display: grid;
  gap: 4px;
  font-size: var(--fs-sm);
  line-height: 1.6;
}
.draftlist b {
  margin-right: var(--s2);
}
</style>
