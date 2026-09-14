<script setup>
/**
 * 这部片子长什么样。弹窗。
 *
 * **画面和风格是一件事，不是两件。** 页面上它们原来是两个各占 365px 的
 * 小节（画幅/清晰度 一个，画风/负向/比例 一个），而用户心里只有一个问题：
 * 这片子长什么样。合成一个弹窗之后，页面上只剩一行答案。
 *
 * ⚠️ **「比例」那四个按钮删掉了，别再加回来。** 它和「画幅」本来是同一件事
 * 存在两个地方：
 *
 *     画幅  changji.toml 的 [video].orientation      → 成片尺寸
 *     比例  assets.json 的 StyleProfile.aspect_ratio → 参考图尺寸
 *
 * 而存画面那个接口一个字都不碰后者，于是「横屏 + 9:16」配得出来也不报错，
 * 出来是参考图竖的、成片横的——参考图正是每一镜的底子。
 *
 * 2026-09-14 引擎那边改成派生：画幅是唯一的源，存画面时顺手把那份拷贝写对
 * （见 config::VideoConfig::aspect_ratio）。`/api/style` 现在**不收**
 * aspect_ratio，发过去是 422。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'
import { VIDEO_QUALITIES, qualitySize } from '@/api/labels'
import { useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const props = defineProps({ open: { type: Boolean, default: false } })
const emit = defineEmits(['close', 'saved'])

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

const video = ref(null)
const style = ref({ global_style: '', negative_prompt: '' })
const styleLine = ref('')
const resetOnStyle = ref(false)
/**
 * 成片工序：后期链（胶片 / 干净 / 关）和声音几层（环境声、配乐）。
 * 剧的属性，存在项目的 changji.toml 的 [look] / [sound]。
 * 配乐还要机器上配了生成命令（music_ready），没配的话勾了也出不来，
 * 所以那个勾旁边要说清。
 */
const finish = ref(null)

const savedVideo = ref('')
const savedStyle = ref('')
const savedFinish = ref('')
const dirty = computed(
  () =>
    (video.value && JSON.stringify(video.value) !== savedVideo.value) ||
    JSON.stringify(style.value) !== savedStyle.value ||
    (finish.value && JSON.stringify(finish.value) !== savedFinish.value),
)

const sizeText = computed(() =>
  video.value ? `${video.value.width}×${video.value.height}` : '—',
)

async function load() {
  if (!session.projectPath) return
  try {
    const d = await api.projectVideo(session.projectPath)
    video.value = { ...d }
  } catch (err) {
    // 读不到不该让弹窗开不了——项目可能是老的，还没有这一节。
    // 按默认显示，用户存一次就写进去了。
    video.value = { orientation: 'portrait', quality: '720p', width: 544, height: 928 }
    ui.warn(`读不到画面设置，按默认显示：${err.message}`)
  }
  savedVideo.value = JSON.stringify(video.value)

  try {
    const data = await api.assets(session.projectPath)
    styleLine.value = data.style?.style_line ?? ''
    style.value = {
      global_style: data.style?.global_style ?? '',
      negative_prompt: data.style?.negative_prompt ?? '',
    }
  } catch {
    // 刚建好还没有资产库。两个空框，存一次就有了。
    style.value = { global_style: '', negative_prompt: '' }
    // 这一句也要跟着清：不清的话，上一部剧读到的「动漫线 / 写实线」
    // 会顶在这一部的标题旁边，而这一部根本没读出来。
    styleLine.value = ''
  }
  savedStyle.value = JSON.stringify(style.value)
  resetOnStyle.value = false

  try {
    const f = await api.projectFinish(session.projectPath)
    finish.value = {
      preset: f.look?.preset ?? 'film',
      ambient: f.sound?.ambient ?? true,
      music: f.sound?.music ?? true,
      music_ready: !!f.sound?.music_ready,
    }
  } catch {
    // 老引擎没有这条接口。不显示这一块，别拦着改画幅。
    finish.value = null
  }
  savedFinish.value = JSON.stringify(finish.value)
}

watch(() => props.open, (now) => now && load())
/**
 * Esc 关掉。
 *
 * 抽屉那几处早就有（「Esc 是唯一不用先瞄准的出口」），而**弹窗比抽屉更该
 * 有**——它盖住整屏，除了右上角那个 ✕ 和点外面，没别的出路。三个弹窗原来
 * 一个都不认 Esc。
 *
 * 判 `props.open`：这个组件是**一直挂着**的（`v-if` 在模板里面），不判的话
 * 它在窗口关着的时候也吃 Esc。
 */
function onEsc(e) {
  if (e.key === 'Escape' && props.open) tryClose()
}
onMounted(() => window.addEventListener('keydown', onEsc))
onUnmounted(() => window.removeEventListener('keydown', onEsc))
/**
 * 打开之后把焦点放进来。
 *
 * **不然键盘上这个弹窗基本没法用。** 用 Tab 走到那颗按钮、回车打开，焦点
 * 还停在**遮罩后面**那颗按钮上：按 Tab 是在看不见的页面里一格格走，走到
 * 弹窗里之前，屏幕上一个焦点框都看不见。
 *
 * 焦点落在面板本身（tabindex="-1"），不猜第一个该聚焦的控件——猜错了会把
 * 人直接丢进某个输入框，而读屏也该先听见这是个什么窗。再按 Tab 就顺着进
 * 里面的控件了。
 */
const panel = ref(null)
// **盯着面板出现，不是盯着 open 变真。** 面板挂在内容上（拍摄那个是
// `v-if="video"`，模型那个是 `v-if="g"`），而内容是打开之后异步读回来的
// ——按 open 那一刻去聚焦会扑空，而扑空的表现正是这条要修的：焦点留在
// 遮罩后面。模板 ref 本身是响应式的，元素挂上来就聚焦，卸了就是 null。
watch(panel, (el) => el?.focus())

/**
 * 关掉之后把焦点还回去。
 *
 * 不还的话焦点落在 `<body>` 上：下一次按 Tab 是从整页开头重走，而人刚才
 * 站在页面中间那颗按钮上。开的时候记一下是谁把它叫起来的，关的时候还给它
 * ——那颗按钮已经不在了（比如刚被这次操作删掉）也没关系，focus 一个不在
 * 文档里的元素什么都不会发生。
 */
let opener = null
watch(
  () => props.open,
  (now) => {
    if (now) {
      opener = document.activeElement
      return
    }
    const back = opener
    opener = null
    back?.focus?.()
  },
)

/**
 * 关掉之前问一句。
 *
 * 这儿本来就有 `dirty`，但只拿去控制保存按钮的禁用——点一下弹窗外面、
 * 或者右上角那个 ×，改的画风和负向就没了，一声不吭。镜头抽屉和角色抽屉
 * 早有这道拦截（「这一镜有改动还没保存，关掉就没了」），措辞照它们。
 */
function tryClose() {
  if (dirty.value && !confirm('画面和风格有改动还没保存，关掉就没了。确定？')) return
  emit('close')
}

async function save() {
  const v = await run(
    () =>
      api.saveProjectVideo({
        path: session.projectPath,
        orientation: video.value.orientation,
        quality: video.value.quality,
      }),
    { key: 'video' },
  )
  if (!v) return
  // **拿服务端算出来的宽高回填。** 前端不该自己算——那样两处规则会漂，
  // 而 32 对齐这种事错了要到出图那一步才发现。
  video.value = { ...v }

  const r = await run(
    () =>
      api.saveStyle({
        project: session.projectPath,
        patch: style.value,
        reset_shots: resetOnStyle.value,
      }),
    { key: 'style', refresh: true },
  )
  if (!r) return

  if (finish.value && JSON.stringify(finish.value) !== savedFinish.value) {
    const f = await run(
      () =>
        api.saveProjectFinish({
          path: session.projectPath,
          look: { preset: finish.value.preset },
          sound: { ambient: finish.value.ambient, music: finish.value.music },
        }),
      { key: 'finish' },
    )
    if (!f) return
  }

  ui.ok(r.reset_shots ? `存好了，${r.reset_shots} 个镜头退回重跑` : '存好了')
  emit('saved')
  emit('close')
}
</script>

<template>
  <div v-if="open" class="mask" @click.self="tryClose">
    <section v-if="video" ref="panel" class="dlg" tabindex="-1">
      <header class="dlg__head">
        <h2 class="dlg__t">这部片子长什么样</h2>
        <span class="pill pill--neutral tiny">
          {{ styleLine === 'anime' ? '动漫线' : '写实线' }}
        </span>
        <span class="spacer" />
        <button class="btn btn--ghost btn--sm" type="button" aria-label="关闭" @click="tryClose">
          <AppIcon name="close" :size="14" />
        </button>
      </header>

      <div class="dlg__body stack stack--sm">
        <div class="two">
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
                 字符串，改了名老项目就读不出来。显示的是真实尺寸，而尺寸
                 按当前画幅算——写死的话横屏项目看到的数全是反的。 -->
            <select v-model="video.quality" class="select" :title="'出来是 ' + sizeText">
              <option v-for="q in VIDEO_QUALITIES" :key="q.value" :value="q.value">
                {{ q.label }} {{ qualitySize(q.value, video.orientation) }} · {{ q.note }}
              </option>
            </select>
          </label>
        </div>

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
            rows="2"
            placeholder="多手多脚，文字水印，糊脸"
          />
        </label>

        <label class="switch" title="不勾的话新设置只对之后才跑的镜头生效">
          <input v-model="resetOnStyle" type="checkbox" />
          <span>已出的镜头退回重跑</span>
        </label>

        <!-- 成片工序：装配时做的那几道。改了下一次装配就生效，不用重出镜头。 -->
        <template v-if="finish">
          <div class="two">
            <label class="field">
              <span class="field__label">后期</span>
              <select v-model="finish.preset" class="select" title="装配时逐镜做：柔化、调色、颗粒">
                <option value="film">胶片 · 柔化、调色、颗粒</option>
                <option value="clean">干净 · 只柔化和颗粒</option>
                <option value="off">关 · 一个滤镜都不加</option>
              </select>
            </label>
            <div class="field">
              <span class="field__label">声音</span>
              <label class="switch" title="出片模型自己出的环境声和动效，压在台词底下">
                <input v-model="finish.ambient" type="checkbox" />
                <span>环境声</span>
              </label>
              <label
                class="switch"
                :title="finish.music_ready ? '一集一条器乐，压在台词底下' : '这台机器还没配配乐命令（全局配置 [sound].music_command），勾了也出不来'"
              >
                <input v-model="finish.music" type="checkbox" />
                <span>配乐<span v-if="!finish.music_ready" class="warn">（机器上没配）</span></span>
              </label>
            </div>
          </div>
        </template>
      </div>

      <footer class="dlg__foot">
        <span class="spacer" />
        <!-- 存是**三趟**：画面、风格、成片工序。这儿原来只挡前两趟——
             第三趟在路上时按钮已经变回可点，而 `dirty` 也还是真（saved*
             要到弹窗关掉重开才刷新）。连点两下，带着「重置镜头」的那趟
             风格保存就又发了一遍，而那一下是要把已渲染的镜头退回重跑的。 -->
        <button
          class="btn btn--primary btn--sm"
          type="button"
          :disabled="!dirty || isBusy('video') || isBusy('style') || isBusy('finish')"
          @click="save"
        >
          {{ isBusy('video') || isBusy('style') || isBusy('finish') ? '存着…' : '保存' }}
        </button>
      </footer>
    </section>
  </div>
</template>

<style scoped>
.mask {
  position: fixed;
  inset: 0;
  z-index: 80;
  display: grid;
  place-items: center;
  padding: var(--s3);
  background: rgb(0 0 0 / 45%);
}

.dlg {
  width: min(560px, 100%);
  max-height: 86vh;
  display: flex;
  flex-direction: column;
  border: 1px solid var(--line);
  border-radius: 12px;
  background: var(--surface);
  box-shadow: 0 20px 60px rgb(0 0 0 / 35%);
}

.dlg__head,
.dlg__foot {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 14px;
}

.dlg__head {
  border-bottom: 1px solid var(--line);
}

.dlg__foot {
  border-top: 1px solid var(--line);
}

.dlg__t {
  margin: 0;
  font-size: var(--fs-md);
  font-weight: 600;
}

.dlg__body {
  padding: 14px;
  overflow-y: auto;
}

.two {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px;
}

@media (max-width: 520px) {
  .two {
    grid-template-columns: 1fr;
  }
}
</style>
