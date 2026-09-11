<script setup>
/**
 * 第八步：上传至平台。
 *
 * 各家平台的开放接口都要资质和密钥，这里不假装能替用户登录。
 * 做的是投递：把成片和一份填好的元数据放到目标目录，或者 POST 给
 * 用户自己配的 webhook，由那边的脚本或第三方工具接手上传。
 */
import { computed, onMounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { humanAgo, humanTime, useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const emit = defineEmits(['go'])
const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

const platforms = ref([])
const targets = ref([])
const records = ref([])
const files = ref([])
const assets = ref(null)

// 单条还是一批。整季跑完是八集八条片，一条一条填标题投出去，
// 投到第五条人就开始出错——投重、漏投、标题串集。
const mode = ref('one')
const batchRels = ref(new Set())
const titleTemplate = ref('')

const editing = ref(null) // 正在编辑的投递目标
const form = ref({
  rel: '',
  targetId: '',
  title: '',
  description: '',
  tagsText: '',
})

const currentTarget = computed(
  () => targets.value.find((t) => t.id === form.value.targetId) ?? null,
)
const currentPlatform = computed(
  () => platforms.value.find((p) => p.id === currentTarget.value?.platform) ?? null,
)
const currentFile = computed(() => files.value.find((f) => f.rel === form.value.rel) ?? null)
const tags = computed(() =>
  form.value.tagsText
    .split(/[\s,，#]+/)
    .map((t) => t.trim())
    .filter(Boolean)
    .slice(0, 20),
)

/**
 * 发片前的自检。
 *
 * 画幅和时长是最常被平台退回来的两项，而这两项在投递之前就能算出来。
 * 等平台退回来才知道，一条片子白等半小时。
 */
const warnings = computed(() => {
  const out = []
  const p = currentPlatform.value
  if (!p) return out
  const ratio = assets.value?.style?.aspect_ratio
  if (p.ratio && ratio && p.ratio !== ratio) {
    out.push(`平台要 ${p.ratio}，项目是 ${ratio}`)
  }
  const planned = session.counters.plannedDurationS
  if (p.maxDurationS && planned && planned > p.maxDurationS) {
    out.push(`约 ${humanTime(planned)}，超过上限 ${humanTime(p.maxDurationS)}`)
  }
  if (!form.value.title.trim()) out.push('标题为空')
  return out
})

async function loadAll() {
  try {
    const [ps, ts] = await Promise.all([api.platforms(), api.publishTargets()])
    platforms.value = ps.platforms ?? []
    targets.value = ts.targets ?? []
    if (!form.value.targetId && targets.value.length) {
      form.value.targetId = targets.value[0].id
    }
  } catch (err) {
    ui.error(err.message)
  }
  if (!session.projectPath) return
  try {
    const [out, rec, ast] = await Promise.all([
      api.outputs(session.projectPath),
      api.publishRecords(session.projectPath),
      api.assets(session.projectPath),
    ])
    files.value = out.files ?? []
    records.value = rec.records ?? []
    assets.value = ast
    if (!form.value.rel) {
      const mine = session.episodeId
        ? files.value.find((f) => f.name.includes(session.episodeId))
        : null
      form.value.rel = (mine ?? files.value[0])?.rel ?? ''
    }
  } catch (err) {
    ui.error(err.message)
  }
}

onMounted(loadAll)
watch(() => [session.projectPath, session.episodeId], loadAll)

// 标题没填过就拿集名兜底。用户改过之后不再覆盖。
watch(
  () => session.episode,
  (ep) => {
    if (ep && !form.value.title) form.value.title = ep.title || ''
    if (ep && !form.value.description) form.value.description = ep.synopsis || ''
  },
  { immediate: true },
)

function newTarget() {
  editing.value = {
    id: '',
    name: '',
    platform: platforms.value[0]?.id ?? 'douyin',
    exportDir: '',
    webhookUrl: '',
    note: '',
  }
}

async function saveTarget() {
  const result = await run(() => api.savePublishTarget(editing.value), {
    key: 'target',
    success: '投递目标已保存',
  })
  if (!result) return
  targets.value = result.targets
  form.value.targetId = result.target.id
  editing.value = null
}

async function removeTarget(id) {
  if (!confirm('删掉这个投递目标？已经投出去的记录还留着。')) return
  const result = await run(() => api.deletePublishTarget(id), { key: 'delTarget' })
  if (!result) return
  targets.value = result.targets
  if (form.value.targetId === id) form.value.targetId = targets.value[0]?.id ?? ''
}

async function deliver() {
  if (!form.value.rel || !form.value.targetId) {
    ui.warn('先挑一条成片和一个投递目标')
    return
  }
  const result = await run(
    () =>
      api.deliver({
        project: session.projectPath,
        episodeId: session.episodeId,
        rel: form.value.rel,
        targetId: form.value.targetId,
        title: form.value.title,
        description: form.value.description,
        tags: tags.value,
      }),
    { key: 'deliver', refresh: true },
  )
  if (!result) return
  ui.ok(result.record.detail || '已投递')
  const rec = await api.publishRecords(session.projectPath)
  records.value = rec.records ?? []
}

function toggleBatch(rel) {
  const next = new Set(batchRels.value)
  if (next.has(rel)) next.delete(rel)
  else next.add(rel)
  batchRels.value = next
}

function toggleBatchAll() {
  batchRels.value =
    batchRels.value.size === files.value.length
      ? new Set()
      : new Set(files.value.map((f) => f.rel))
}

async function deliverBatch() {
  if (!batchRels.value.size || !form.value.targetId) {
    ui.warn('先勾几条片，再挑一个投递目标')
    return
  }
  const result = await run(
    () =>
      api.deliverBatch({
        project: session.projectPath,
        targetId: form.value.targetId,
        // 按片单顺序投，不按勾选顺序——投递目录里文件名带时间戳，
        // 乱序投出去在那边排出来是乱的
        rels: files.value.map((f) => f.rel).filter((r) => batchRels.value.has(r)),
        titleTemplate: titleTemplate.value.trim(),
        tags: tags.value,
      }),
    { key: 'batch', refresh: true },
  )
  if (!result) return
  ui.ok(
    result.failed
      ? `投了 ${result.delivered} 条，${result.failed} 条失败，看下面的记录`
      : `${result.delivered} 条都投出去了`,
  )
  batchRels.value = new Set()
  const rec = await api.publishRecords(session.projectPath)
  records.value = rec.records ?? []
}

const platformName = (id) => platforms.value.find((p) => p.id === id)?.name ?? id

/**
 * 失败的记录再投一次。
 *
 * 失败大多是投递目录不在了或者 webhook 那头没起来，修好之后重来一次
 * 就好。把当时填的标题简介重新打一遍是没道理的，记录里都存着。
 */
function retry(record) {
  form.value = {
    rel: record.source,
    targetId: record.targetId,
    title: record.title,
    description: record.description,
    tagsText: (record.tags ?? []).join(' '),
  }
  if (!targets.value.some((t) => t.id === record.targetId)) {
    ui.warn('原来那个投递目标已经删了，先挑一个新的')
    return
  }
  if (!files.value.some((f) => f.rel === record.source)) {
    ui.warn('原来那条成片不在了，先挑一条新的')
    return
  }
  deliver()
}
</script>

<template>
  <div class="stack stack--lg">
    <div class="toolbar">
      <div v-if="session.hasProject && files.length" class="chips">
        <button
          v-for="m in [
            { v: 'one', l: '单条' },
            { v: 'batch', l: '批量' },
          ]"
          :key="m.v"
          class="chip"
          :class="{ 'chip--on': mode === m.v }"
          type="button"
          @click="mode = m.v"
        >
          {{ m.l }}
        </button>
      </div>
      <span class="spacer" />
      <button class="btn btn--ghost btn--sm" type="button" @click="newTarget">
        <AppIcon name="plus" :size="14" />
        加目标
      </button>
    </div>

    <EmptyState v-if="!session.hasProject" icon="folder" tone="warn" title="还没选项目">
      <RouterLink to="/project" class="btn btn--primary">去第一步</RouterLink>
    </EmptyState>

    <EmptyState v-else-if="!files.length" icon="film" title="还没有能投的片" hint="先把制作跑完">
      <button class="btn btn--primary" type="button" @click="emit('go', 'shots')">
        去做镜头
      </button>
    </EmptyState>

    <template v-else>
      <div class="pub">
        <!-- 投递表单 -->
        <section class="sec stack">
          <label v-if="mode === 'one'" class="field">
            <span class="field__label">成片</span>
            <select v-model="form.rel" class="select">
              <option v-for="f in files" :key="f.rel" :value="f.rel">
                {{ f.name }}（{{ f.size_mb }} MB）
              </option>
            </select>
          </label>

          <div v-else class="field">
            <span class="field__label">
              成片
              <button class="linkbtn tiny" type="button" @click="toggleBatchAll">
                {{ batchRels.size === files.length ? '全不选' : '全选' }}
              </button>
            </span>
            <div class="picklist">
              <label v-for="f in files" :key="f.rel" class="pick">
                <input
                  type="checkbox"
                  :checked="batchRels.has(f.rel)"
                  @change="toggleBatch(f.rel)"
                />
                <span class="truncate">{{ f.name }}</span>
                <span class="tiny dim numeric nowrap">{{ f.size_mb }} MB</span>
              </label>
            </div>
          </div>

          <label class="field">
            <span class="field__label">目标</span>
            <select
              v-model="form.targetId"
              class="select"
              :title="currentTarget ? currentTarget.exportDir || currentTarget.webhookUrl : ''"
            >
              <option v-if="!targets.length" value="">还没有投递目标</option>
              <option v-for="t in targets" :key="t.id" :value="t.id">
                {{ t.name }} · {{ platformName(t.platform) }}
              </option>
            </select>
          </label>

          <label v-if="mode === 'one'" class="field">
            <span class="field__label">标题</span>
            <input v-model="form.title" class="input" maxlength="120" />
          </label>

          <label v-else class="field">
            <span class="field__label">标题模板</span>
            <input
              v-model="titleTemplate"
              class="input"
              placeholder="留空用集名；可用 {title} 集名 {episode} 集号 {index} 序号"
            />
          </label>

          <label v-if="mode === 'one'" class="field">
            <span class="field__label">简介</span>
            <textarea v-model="form.description" class="textarea" rows="4" maxlength="2000" />
          </label>

          <label class="field">
            <span class="field__label">话题</span>
            <input v-model="form.tagsText" class="input" placeholder="空格或逗号分隔" />
            <span v-if="tags.length" class="row row--wrap" style="margin-top: 6px">
              <span v-for="t in tags" :key="t" class="pill pill--neutral">#{{ t }}</span>
            </span>
          </label>

          <div v-if="mode === 'one' && warnings.length" class="warns">
            <p v-for="(w, i) in warnings" :key="i" class="warns__row">
              <AppIcon name="warn" :size="14" />
              <span>{{ w }}</span>
            </p>
          </div>

          <div class="row">
            <button
              v-if="mode === 'one'"
              class="btn btn--primary"
              type="button"
              title="放进目标目录或 POST 给 webhook，不替你登录平台"
              :disabled="!targets.length || isBusy('deliver')"
              @click="deliver"
            >
              <AppIcon name="upload" :size="15" />
              {{ isBusy('deliver') ? '投递中…' : '投递' }}
            </button>
            <button
              v-else
              class="btn btn--primary"
              type="button"
              title="放进目标目录或 POST 给 webhook，不替你登录平台；逐条投，一条失败不影响其余"
              :disabled="!targets.length || !batchRels.size || isBusy('batch')"
              @click="deliverBatch"
            >
              <AppIcon name="upload" :size="15" />
              {{ isBusy('batch') ? `投 ${batchRels.size} 条中…` : `投递 ${batchRels.size} 条` }}
            </button>
            <span class="spacer" />
            <span v-if="mode === 'one' && currentFile" class="tiny dim numeric">
              {{ currentFile.size_mb }} MB
            </span>
          </div>
        </section>

        <!-- 侧栏：预览 + 目标 -->
        <div class="stack">
          <video
            v-if="currentFile"
            class="pub__video"
            :src="mediaUrl(session.projectPath, currentFile.rel)"
            controls
            preload="metadata"
            playsinline
          />

          <section class="sec">
            <div class="sec__head">
              <h2 class="sec__t">投递目标</h2>
              <span class="spacer" />
              <div class="sec__acts">
                <button class="btn btn--ghost btn--sm" type="button" @click="newTarget">
                  <AppIcon name="plus" :size="14" />
                </button>
              </div>
            </div>
            <p v-if="!targets.length" class="tiny dim">还没有目标</p>
            <div v-else class="tgt__list">
              <div v-for="t in targets" :key="t.id" class="tgt">
                <span class="tgt__text">
                  <span class="tgt__name">{{ t.name }}</span>
                  <span class="tiny dim truncate">
                    {{ platformName(t.platform) }} ·
                    {{ t.exportDir || t.webhookUrl }}
                  </span>
                </span>
                <button class="btn btn--ghost btn--sm" type="button" @click="editing = { ...t }">
                  改
                </button>
                <button
                  class="btn btn--ghost btn--sm"
                  type="button"
                  @click="removeTarget(t.id)"
                >
                  <AppIcon name="trash" :size="13" />
                </button>
              </div>
            </div>
          </section>
        </div>
      </div>

      <!-- 投递记录 -->
      <section v-if="records.length" class="sec">
        <div class="sec__head">
          <h2 class="sec__t">投递记录</h2>
          <span class="tiny dim">{{ records.length }} 条</span>
        </div>
        <div class="rec__list">
          <div v-for="r in records" :key="r.id" class="rec">
            <span class="pill" :class="r.status === 'done' ? 'pill--ok' : 'pill--danger'">
              {{ r.status === 'done' ? '已投' : '失败' }}
            </span>
            <span class="rec__text">
              <span class="truncate">{{ r.title || r.source }}</span>
              <span class="tiny dim truncate">
                {{ r.targetName }} · {{ platformName(r.platform) }} · {{ r.detail }}
              </span>
            </span>
            <button
              v-if="r.status !== 'done'"
              class="btn btn--ghost btn--sm"
              type="button"
              :disabled="isBusy('deliver')"
              @click="retry(r)"
            >
              <AppIcon name="refresh" :size="13" />
              重试
            </button>
            <span class="tiny dim nowrap">{{ humanAgo(r.deliveredAt) }}</span>
          </div>
        </div>
      </section>
    </template>

    <!-- 目标编辑 -->
    <div v-if="editing" class="modal" @click.self="editing = null">
      <div class="modal__box card">
        <div class="card__head">
          <div class="card__title">{{ editing.id ? '改投递目标' : '加投递目标' }}</div>
          <button class="btn btn--ghost btn--sm" type="button" @click="editing = null">
            <AppIcon name="close" :size="14" />
          </button>
        </div>
        <div class="card__body stack">
          <label class="field">
            <span class="field__label">名字</span>
            <input v-model="editing.name" class="input" placeholder="例如：抖音主号" />
          </label>
          <label class="field">
            <span class="field__label">平台</span>
            <select v-model="editing.platform" class="select">
              <option v-for="p in platforms" :key="p.id" :value="p.id">
                {{ p.name }}{{ p.ratio ? `（${p.ratio}）` : '' }}
              </option>
            </select>
          </label>
          <label class="field">
            <span class="field__label">投递目录</span>
            <input
              v-model="editing.exportDir"
              class="input mono"
              placeholder="D:\待发布\抖音"
              title="视频和同名 json 元数据一起放进去"
            />
          </label>
          <label class="field">
            <span class="field__label">webhook</span>
            <input
              v-model="editing.webhookUrl"
              class="input mono"
              placeholder="https://…（可选）"
              title="投递时把元数据 POST 到这个地址"
            />
          </label>
          <label class="field">
            <span class="field__label">备注</span>
            <input v-model="editing.note" class="input" />
          </label>
        </div>
        <div class="card__foot">
          <button
            class="btn btn--primary"
            type="button"
            :disabled="isBusy('target')"
            @click="saveTarget"
          >
            保存
          </button>
          <button class="btn btn--ghost btn--sm" type="button" @click="editing = null">取消</button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.pub {
  display: grid;
  grid-template-columns: minmax(0, 1.15fr) minmax(0, 0.85fr);
  gap: var(--s5);
  align-items: start;
}

.pub__video {
  display: block;
  width: 100%;
  max-height: 340px;
  border-radius: var(--r);
  background: #000;
}

.chips {
  display: flex;
  gap: 6px;
}
.chip {
  padding: 3px var(--s3);
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

.linkbtn {
  border: none;
  background: none;
  padding: 0;
  color: var(--accent);
  cursor: pointer;
  font-weight: 500;
}
.linkbtn:hover {
  text-decoration: underline;
}

.picklist {
  display: flex;
  flex-direction: column;
  max-height: 220px;
  overflow-y: auto;
  border: 1px solid var(--line);
  border-radius: var(--r);
  background: var(--bg-sunken);
}
.pick {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s2) var(--s3);
  border-bottom: 1px solid color-mix(in srgb, var(--line) 60%, transparent);
  font-size: var(--fs-base);
  cursor: pointer;
}
.pick:last-child {
  border-bottom: none;
}
.pick:hover {
  background: var(--surface-2);
}
.pick input {
  width: 15px;
  height: 15px;
  accent-color: var(--accent);
  flex: none;
}
.pick > span:first-of-type {
  flex: 1;
  min-width: 0;
}

.warns {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: var(--s2) var(--s3);
  border-radius: var(--r);
  background: var(--warn-soft);
}
.warns__row {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin: 0;
  color: var(--warn);
  font-size: var(--fs-sm);
  line-height: 1.5;
}

.tgt__list,
.rec__list {
  display: flex;
  flex-direction: column;
}
.tgt,
.rec {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s2) 0;
  border-bottom: 1px solid var(--line);
}
.tgt:last-child,
.rec:last-child {
  border-bottom: none;
}
.tgt__text,
.rec__text {
  display: flex;
  flex-direction: column;
  min-width: 0;
  flex: 1;
  line-height: 1.3;
}
.tgt__name {
  font-size: var(--fs-base);
  font-weight: 600;
}

/* .modal 提到 styles/base.css 了——它被抄过三遍。 */
.modal__box {
  width: min(460px, 100%);
  box-shadow: var(--shadow-3);
}

@media (max-width: 980px) {
  .pub {
    grid-template-columns: 1fr;
  }
}
</style>
