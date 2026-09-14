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
import { computed, ref, watch } from 'vue'

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
  <div v-if="open" class="mask" @click.self="emit('close')">
    <section v-if="video" class="dlg">
      <header class="dlg__head">
        <h2 class="dlg__t">这部片子长什么样</h2>
        <span class="pill pill--neutral tiny">
          {{ styleLine === 'anime' ? '动漫线' : '写实线' }}
        </span>
        <span class="spacer" />
        <button class="btn btn--ghost btn--sm" type="button" @click="emit('close')">
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
        <button
          class="btn btn--primary btn--sm"
          type="button"
          :disabled="!dirty || isBusy('video') || isBusy('style')"
          @click="save"
        >
          {{ isBusy('video') || isBusy('style') ? '存着…' : '保存' }}
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
