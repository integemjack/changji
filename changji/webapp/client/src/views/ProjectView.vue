<script setup>
/**
 * 这一部剧。
 *
 * **这一页只说当前这个项目。** 选哪一部在常驻的项目库里。这一页显示的是：
 * 这是哪部剧（logline）、到哪一步了、这一部剧自己的设置（画面、全剧风格）、
 * 以及删掉它。
 *
 * 画面和全剧风格**是项目级的**——一台机器上可以同时有竖屏短剧和横屏片子，
 * 所以它们不能回全局设置页。
 *
 * 2026-09-11 起照故事页的样子：没有页头、没有卡片、没有解释性的段落。
 * 一行读数，两块表单并排，删项目是最底下一行字。
 */
import { computed, onMounted, ref, watch } from 'vue'

import EmptyState from '@/components/EmptyState.vue'
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
    // 项目坏了或者刚建好还没有资产库，这一块不显示就是了
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
  <!-- 根元素的类名不能叫 .proj：子组件的根会带上父组件的 scoped 属性，
       App.vue 里给顶栏项目按钮写的 .proj { max-width: 11rem } 会套到这儿，
       整页被压成 176px 宽。栽过一次。 -->
  <div class="pj">
    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="在项目库里点一个"
    />

    <template v-else>
      <!-- 这是哪部剧、到哪一步了：一行读数 -->
      <section class="sec">
        <p class="lead" :class="{ dim: !me?.logline }">{{ me?.logline || '还没写故事' }}</p>
        <div class="meta tiny dim">
          <span v-if="stage" class="stage" :class="`stage--${stage.tone}`">{{ stage.label }}</span>
          <span><b class="numeric">{{ me?.chapters ?? 0 }}</b> 章</span>
          <span><b class="numeric">{{ me?.episodes ?? 0 }}</b> 集</span>
          <span><b class="numeric">{{ me?.shots ?? 0 }}</b> 镜</span>
          <span><b class="numeric">{{ me?.outputs ?? 0 }}</b> 成片</span>
          <span>{{ humanAgo(me?.mtime) }}</span>
          <span class="mono truncate">{{ session.projectPath }}</span>
        </div>
        <div v-if="stage" class="bar">
          <div
            class="bar__fill"
            :class="`bar__fill--${stage.tone}`"
            :style="{ width: stage.percent + '%' }"
          />
        </div>
      </section>

      <div class="cols">
        <!-- 画面 -->
        <section v-if="video" class="sec">
          <div class="sec__head">
            <h2 class="sec__t">画面</h2>
            <span v-if="videoDirty" class="pill pill--warn">未存</span>
            <span class="spacer" />
            <button
              class="btn btn--primary btn--sm"
              type="button"
              :disabled="!videoDirty || isBusy('video')"
              title="只影响以后出的镜头"
              @click="saveVideo"
            >
              {{ isBusy('video') ? '存着…' : '保存' }}
            </button>
          </div>
          <div class="form">
            <label class="field">
              <span class="field__label">画幅</span>
              <select v-model="video.orientation" class="select">
                <option value="portrait">竖屏</option>
                <option value="landscape">横屏</option>
              </select>
            </label>
            <label class="field">
              <span class="field__label">清晰度</span>
              <!-- **取值仍然是 "720p"**：那是存在每个项目 changji.toml 里的
                   字符串，改了名老项目就读不出来。显示的是真实尺寸。 -->
              <select v-model="video.quality" class="select" :title="'出来是 ' + sizeText">
                <option value="720p">标准 544×928 · 快</option>
                <option value="hd">高清 704×1280 · 推荐</option>
                <option value="2k">2K 2560×1440 · 很吃显存</option>
              </select>
            </label>
          </div>
        </section>

        <!-- 全剧风格 -->
        <section v-if="savedStyle" class="sec">
          <div class="sec__head">
            <h2 class="sec__t">全剧风格</h2>
            <span class="pill pill--neutral">{{ styleLine === 'anime' ? '动漫线' : '写实线' }}</span>
            <span v-if="styleDirty" class="pill pill--warn">未存</span>
            <span class="spacer" />
            <button
              class="btn btn--primary btn--sm"
              type="button"
              :disabled="!styleDirty || isBusy('style')"
              @click="saveStyle"
            >
              {{ isBusy('style') ? '存着…' : '保存' }}
            </button>
          </div>
          <div class="form">
            <label class="field">
              <span class="field__label">画风</span>
              <textarea
                v-model="style.global_style"
                class="textarea textarea--tight"
                rows="3"
                placeholder="电影感冷调，浅景深，胶片颗粒"
              />
            </label>
            <label class="field">
              <span class="field__label">负向</span>
              <textarea
                v-model="style.negative_prompt"
                class="textarea textarea--tight"
                rows="3"
                placeholder="多手多脚，文字水印，糊脸"
              />
            </label>
          </div>
          <div class="row row--wrap">
            <div class="chips" title="改画幅要重跑所有镜头">
              <button
                v-for="r in ['9:16', '16:9', '1:1', '4:5']"
                :key="r"
                class="chip"
                :class="{ 'chip--on': style.aspect_ratio === r }"
                type="button"
                @click="style.aspect_ratio = r"
              >
                {{ r }}
              </button>
            </div>
            <label class="switch" title="不勾的话新风格只对之后才跑的镜头生效">
              <input v-model="resetOnStyle" type="checkbox" />
              <span>已出的镜头退回重跑</span>
            </label>
          </div>
        </section>
      </div>

      <!-- 删掉这个项目。一行字，点开才有确认框。不可逆，摆在最底下。 -->
      <section class="sec">
        <div class="row row--wrap">
          <button
            class="btn btn--ghost btn--sm"
            :class="{ 'danger-text': !removing }"
            type="button"
            @click="removing = !removing"
          >
            {{ removing ? '算了' : '删掉这个项目' }}
          </button>
          <template v-if="removing">
            <input
              v-model="confirmName"
              class="input confirm"
              :placeholder="'照着打一遍「' + title + '」'"
            />
            <button
              class="btn btn--danger btn--sm"
              type="button"
              :disabled="confirmName !== title || isBusy('delete')"
              @click="remove"
            >
              永久删除
            </button>
          </template>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.pj {
  display: flex;
  flex-direction: column;
}
.lead {
  margin: 0 0 var(--s2);
  font-size: var(--fs-md);
  line-height: 1.6;
}
.meta {
  display: flex;
  align-items: baseline;
  gap: var(--s3);
  flex-wrap: wrap;
  min-width: 0;
}
.meta b {
  color: var(--text);
  font-size: var(--fs-base);
}
.meta .truncate {
  min-width: 0;
  max-width: 40ch;
}
.stage {
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
.bar {
  margin-top: var(--s2);
  height: 3px;
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

/* 画面和风格并排。窄了就叠。 */
.cols {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
  gap: 0 var(--s8);
}
.form {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: var(--s3);
  margin-bottom: var(--s3);
}
.textarea--tight {
  min-height: 0;
}
.chips {
  display: inline-flex;
  border: 1px solid var(--line);
  border-radius: var(--r-sm);
  overflow: hidden;
}
.chip {
  padding: 4px 10px;
  border: 0;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.chip--on {
  background: var(--accent-soft);
  color: var(--accent);
}
.switch {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: var(--fs-sm);
  color: var(--text-2);
  cursor: pointer;
}
.danger-text {
  color: var(--danger);
}
.confirm {
  width: 18em;
  height: 27px;
}
</style>
