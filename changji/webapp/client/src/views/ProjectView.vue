<script setup>
/**
 * 这一部剧。
 *
 * **这一页只说当前这个项目。** 选哪一部在右边那条常驻的项目库里——
 * 原来两件事挤在一页：上半页是「这一部的设置」，下半页是「挑另一部」，
 * 而挑另一部这件事在写故事、看镜头的时候也想干，走到这一页再走回去，
 * 当前那一页的状态就丢了。
 *
 * 所以这一页显示的是：这是哪部剧（剧名、logline）、到哪一步了、
 * 这一部剧自己的设置（画面、全剧风格）、以及删掉它。
 *
 * 画面和全剧风格**是项目级的**——一台机器上可以同时有竖屏短剧和横屏
 * 片子，所以它们不能回全局设置页；而这一页就是这一部剧的地方。
 */
import { computed, onMounted, ref, watch } from 'vue'

import EmptyState from '@/components/EmptyState.vue'
import StepHeader from '@/components/StepHeader.vue'
import { projectStage } from '@/composables/project-stage'
import { api } from '@/api'
import { humanAgo } from '@/composables/useAction'
import { useAction } from '@/composables/useAction'
import { useProjects } from '@/stores/projects'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const store = useProjects()
const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

const removing = ref(false)
const confirmName = ref('')

/** 项目库里这一条。统计和阶段都从它来，和右边那条栏读的是同一份。 */
const me = computed(() => store.byPath(session.projectPath))
const stage = computed(() => (me.value ? projectStage(me.value) : null))
const title = computed(
  () => session.project?.title || session.project?.project_id || '',
)

// 全剧风格。所有镜头共用的一层，是这部剧的创作常量：开工时定一次，
// 后面基本不动。改一次会让已经出好的镜头退回重跑，所以下面那个勾要人自己点。
const style = ref({ global_style: '', negative_prompt: '', aspect_ratio: '9:16' })
const savedStyle = ref('')
const styleLine = ref('')
const resetOnStyle = ref(false)
const styleDirty = computed(() => JSON.stringify(style.value) !== savedStyle.value)

const video = ref(null)
const savedVideo = ref('')
const videoDirty = computed(
  () => video.value && JSON.stringify(video.value) !== savedVideo.value,
)
const sizeText = computed(() =>
  video.value ? `${video.value.width}×${video.value.height}` : '—',
)

async function loadVideo() {
  if (!session.projectPath) {
    video.value = null
    return
  }
  try {
    const d = await api.projectVideo(session.projectPath)
    video.value = { ...d }
    savedVideo.value = JSON.stringify(video.value)
  } catch (err) {
    // 读不到不该让整页红——项目可能是老的，还没有这一节。
    // 那时候按默认值显示，用户存一次就写进去了。
    video.value = { orientation: 'portrait', quality: '720p', width: 544, height: 928 }
    savedVideo.value = ''
    ui.warn(`读不到画面设置，按默认显示：${err.message}`)
  }
}

async function saveVideo() {
  const d = await run(
    () =>
      api.saveProjectVideo({
        path: session.projectPath,
        orientation: video.value.orientation,
        quality: video.value.quality,
      }),
    { key: 'video', success: '画面设置已保存' },
  )
  if (!d) return
  // **拿服务端算出来的宽高回填。** 前端不该自己算——那样两处规则会漂，
  // 而 32 对齐这种事错了要到出图那一步才发现。
  video.value = { ...d }
  savedVideo.value = JSON.stringify(video.value)
}

async function loadStyle() {
  if (!session.projectPath) return
  try {
    const data = await api.assets(session.projectPath)
    styleLine.value = data.style?.style_line ?? ''
    style.value = {
      global_style: data.style?.global_style ?? '',
      negative_prompt: data.style?.negative_prompt ?? '',
      aspect_ratio: data.style?.aspect_ratio ?? '9:16',
    }
    savedStyle.value = JSON.stringify(style.value)
  } catch {
    // 项目坏了或者刚建好还没有资产库，这一张卡不显示就是了
    savedStyle.value = ''
  }
}

async function saveStyle() {
  const result = await run(
    () =>
      api.saveStyle({
        project: session.projectPath,
        patch: style.value,
        reset_shots: resetOnStyle.value,
      }),
    { key: 'style', refresh: true },
  )
  if (!result) return
  savedStyle.value = JSON.stringify(style.value)
  ui.ok(result.reset_shots ? `风格已改，${result.reset_shots} 个镜头退回重跑` : '已保存')
}

async function remove() {
  const path = session.projectPath
  const done = await run(
    () => api.deleteProject({ path, confirm_name: confirmName.value }),
    { key: 'delete', success: '已删除' },
  )
  if (!done) return
  removing.value = false
  confirmName.value = ''
  session.clear()
  await store.load()
}

onMounted(() => {
  if (!store.loaded) store.load()
})

watch(
  () => session.projectPath,
  () => {
    loadVideo()
    loadStyle()
  },
  { immediate: true },
)
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader :title="title || '项目'" />

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="项目库那条栏里点一个切过去，或者点加号建一个。"
    />

    <template v-else>
      <!-- 这是哪部剧、到哪一步了 -->
      <section class="card">
        <div class="card__body stack stack--sm">
          <p v-if="me?.logline" class="lead">{{ me.logline }}</p>
          <p v-else class="lead dim">还没写故事。去「故事」那一步写一句，或者粘一段进来。</p>

          <div v-if="stage" class="row row--between">
            <span class="stage" :class="`stage--${stage.tone}`">{{ stage.label }}</span>
            <span class="tiny dim">{{ humanAgo(me?.mtime) }}</span>
          </div>
          <div v-if="stage" class="bar">
            <div class="bar__fill" :class="`bar__fill--${stage.tone}`"
                 :style="{ width: stage.percent + '%' }" />
          </div>

          <div class="stats tiny dim">
            <span><b class="numeric">{{ me?.chapters ?? 0 }}</b> 章</span>
            <span><b class="numeric">{{ me?.episodes ?? 0 }}</b> 集</span>
            <span><b class="numeric">{{ me?.shots ?? 0 }}</b> 镜</span>
            <span><b class="numeric">{{ me?.outputs ?? 0 }}</b> 成片</span>
          </div>
          <p class="tiny dim mono truncate">{{ session.projectPath }}</p>
        </div>
      </section>

<section v-if="session.hasProject && video" class="card">
      <div class="card__head">
        <div>
          <div class="card__title">
            画面
            <span class="pill pill--neutral">
              {{ video.orientation === 'landscape' ? '横屏' : '竖屏' }}
              · {{ video.quality === '2k' ? '2K' : '标准' }}
            </span>
          </div>
          <div class="card__sub">
            出多大的画面。宽高由这两项算出来，不用自己填数字。
          </div>
        </div>
        <span v-if="videoDirty" class="pill pill--warn">未保存</span>
      </div>
      <div class="card__body stack">
        <div class="grid grid--form">
          <label class="field">
            <span class="field__label">画幅</span>
            <select v-model="video.orientation" class="select">
              <option value="portrait">竖屏（短剧、手机）</option>
              <option value="landscape">横屏</option>
            </select>
          </label>
          <label class="field">
            <span class="field__label">清晰度</span>
            <select v-model="video.quality" class="select">
              <!-- **取值仍然是 "720p"**：那是存在每个项目 changji.toml 里的
                   字符串，改了名老项目就读不出来。显示的是真实尺寸——
                   2026-09-10 把它从 704×1280 改成 544×928 之后，
                   再叫 "720p" 就是假的（720p 是 720 行）。 -->
              <option value="720p">标准（544×928）· 快</option>
              <option value="hd">高清（704×1280）· 推荐</option>
              <option value="2k">2K（2560×1440）· 很吃显存</option>
            </select>
            <span class="field__hint">
              出来是 {{ sizeText }}。
              <template v-if="video.quality === '720p'">
                像素只有高清档的一半多一点，出得快，但细节和人脸容易糊。
              </template>
              <template v-else-if="video.quality === 'hd'">
                <strong>画质和速度的平衡点</strong>，多数情况选它。
              </template>
              <template v-if="video.quality === '2k'">
                <strong>2K 很吃显存</strong>，一张 32 GB 的卡跑不动——
                那时候要么换大卡，要么出标准档再单独走一次超分。
              </template>
            </span>
          </label>
        </div>
      </div>
      <div class="card__foot">
        <button
          class="btn btn--primary"
          type="button"
          :disabled="!videoDirty || isBusy('video')"
          @click="saveVideo"
        >
          保存画面设置
        </button>
        <span class="spacer" />
        <span class="tiny dim">改它只影响以后出的镜头，已经出好的不动</span>
      </div>
    </section>

    
    <section v-if="session.hasProject && savedStyle" class="card">
      <div class="card__head">
        <div>
          <div class="card__title">
            全剧风格
            <span class="pill pill--neutral">
              {{ styleLine === 'anime' ? '动漫线' : '写实线' }}
            </span>
          </div>
          <div class="card__sub">
            所有镜头共用的一层，开工时定一次。改它等于整部剧换调性。
          </div>
        </div>
        <span v-if="styleDirty" class="pill pill--warn">未保存</span>
      </div>
      <div class="card__body stack">
        <div class="grid grid--form">
          <label class="field">
            <span class="field__label">画风与质感</span>
            <textarea
              v-model="style.global_style"
              class="textarea textarea--tight"
              rows="3"
              placeholder="例如：电影感冷调，浅景深，胶片颗粒"
            />
          </label>
          <label class="field">
            <span class="field__label">负向提示词</span>
            <textarea
              v-model="style.negative_prompt"
              class="textarea textarea--tight"
              rows="3"
              placeholder="例如：多手多脚，文字水印，糊脸"
            />
          </label>
        </div>

        <div class="field">
          <span class="field__label">画幅</span>
          <div class="chips">
            <button
              v-for="r in ['9:16', '16:9', '1:1', '4:5']"
              :key="r"
              class="chip"
              :class="{ 'chip--on': style.aspect_ratio === r }"
              type="button"
              @click="style.aspect_ratio = r"
            >
              {{ r }}{{ r === '9:16' ? ' 竖屏' : r === '16:9' ? ' 横屏' : '' }}
            </button>
          </div>
          <span class="field__hint">短剧平台基本都吃 9:16。改画幅要重跑所有镜头。</span>
        </div>

        <label class="switch">
          <input v-model="resetOnStyle" type="checkbox" />
          <span>顺便把已渲染的镜头退回重跑</span>
          <span class="field__hint">
            不勾的话新风格只对之后才跑的镜头生效，一集里前后会不一致。
          </span>
        </label>
      </div>
      <div class="card__foot">
        <button
          class="btn btn--primary"
          type="button"
          :disabled="!styleDirty || isBusy('style')"
          @click="saveStyle"
        >
          {{ isBusy('style') ? '保存中…' : '保存风格' }}
        </button>
      </div>
    </section>

      <!-- 危险区。**摆在最底下、单独一块。** 删项目不可逆，不该和上面
           那些随手改的东西挨着。 -->
      <section class="card card--bad">
        <div class="card__head">
          <div>
            <div class="card__title">删掉这个项目</div>
            <div class="card__sub">
              整个目录连同剧本、分镜、配音、成片一起没，不可撤销。
            </div>
          </div>
          <button class="btn btn--ghost btn--sm" type="button" @click="removing = !removing">
            {{ removing ? '算了' : '我要删' }}
          </button>
        </div>
        <div v-if="removing" class="card__body stack stack--sm">
          <label class="field">
            <span class="field__label">
              照着打一遍项目名「{{ title }}」确认
            </span>
            <input v-model="confirmName" class="input" :placeholder="title" />
          </label>
          <div class="row">
            <button
              class="btn btn--danger"
              type="button"
              :disabled="confirmName !== title || isBusy('delete')"
              @click="remove"
            >
              永久删除
            </button>
          </div>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.lead {
  font-size: var(--fs-md);
  line-height: 1.6;
}
.stage {
  font-size: var(--fs-sm);
  font-weight: 600;
}
.stage--ok {
  color: var(--ok);
}
.stage--warn {
  color: var(--warn);
}
.stage--accent {
  color: var(--accent);
}
.stage--dim,
.stage--bad {
  color: var(--text-3);
  font-weight: 400;
}
.bar {
  height: 4px;
  border-radius: 2px;
  background: var(--surface-2);
  overflow: hidden;
}
.bar__fill {
  height: 100%;
  background: var(--accent);
  transition: width 0.2s var(--ease);
}
.bar__fill--ok {
  background: var(--ok);
}
.bar__fill--warn {
  background: var(--warn);
}
.stats {
  display: flex;
  gap: var(--s4);
}
.stats b {
  color: var(--text);
  font-size: var(--fs-base);
}
</style>
