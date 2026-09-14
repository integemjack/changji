/**
 * 当前在做哪个项目的哪一集。
 *
 * 整个引导流程的上下文就这两个值，存 localStorage。
 * 关掉浏览器第二天回来，还能接着上次那一集往下走。
 */

import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { api } from '@/api'

const KEY_PROJECT = 'changji.project'
const KEY_EPISODE = 'changji.episode'

export const useSession = defineStore('session', () => {
  const projectPath = ref(localStorage.getItem(KEY_PROJECT) || '')
  const episodeId = ref(localStorage.getItem(KEY_EPISODE) || '')

  const project = ref(null) // /api/project 的返回
  const flow = ref(null) // /bff/flow 的返回
  const loading = ref(false)
  const error = ref('')

  const episodes = computed(() => project.value?.episodes ?? [])
  const episode = computed(
    () => episodes.value.find((e) => e.episode_id === episodeId.value) ?? null,
  )
  const characters = computed(() => project.value?.characters ?? [])
  const locations = computed(() => project.value?.locations ?? [])
  const done = computed(() => flow.value?.done ?? {})
  const counters = computed(() => flow.value?.counters ?? {})
  const hasProject = computed(() => Boolean(projectPath.value))

  function selectProject(path) {
    projectPath.value = path
    localStorage.setItem(KEY_PROJECT, path)
    // 换项目时旧的集号一定对不上，先清掉，refresh 会挑第一集
    episodeId.value = ''
    localStorage.removeItem(KEY_EPISODE)
    project.value = null
    flow.value = null
  }

  function selectEpisode(id) {
    episodeId.value = id || ''
    if (id) localStorage.setItem(KEY_EPISODE, id)
    else localStorage.removeItem(KEY_EPISODE)
  }

  function clear() {
    selectProject('')
    projectPath.value = ''
    localStorage.removeItem(KEY_PROJECT)
  }

  /** 重新问一遍项目和流程进度。每一步做完都调它。 */
  async function refresh() {
    if (!projectPath.value) {
      project.value = null
      flow.value = null
      return
    }
    loading.value = true
    error.value = ''
    try {
      const data = await api.flow(projectPath.value, episodeId.value)
      flow.value = data
      project.value = data.project
      // 引擎挑了哪一集就跟着它，省得前端自己再判一遍第一集是谁
      if (data.episodeId && data.episodeId !== episodeId.value) {
        selectEpisode(data.episodeId)
      }
      if (!data.episodeId) selectEpisode('')
    } catch (err) {
      error.value = err.message
      // **说出来。** 这一条原来只写进 error 就完了，而 `session.error`
      // 界面上一处都没读——于是这条路整个是哑的：
      //
      //   · 400/404（项目被删了、路径不对）：下面把 project 清空，
      //     而 hasProject 看的是 projectPath，还是真的。结果每一页都
      //     退成「还没选到某一集 / 顶上挑一集」，顶栏那个集号下拉是空的
      //     ——一个指着用户去做一件做不到的事的提示，真正的原因一个字没有。
      //   · 5xx / 连不上：project 和 flow 都留着上一份，页面照旧画着旧数据，
      //     而侧边栏的对勾、集号、进度全停在上一次成功那一刻。
      //
      // 走 `changji:error` 这条自定义事件，和 router.onError 同一个口子
      // （见 router/index.js）：ToastStack 订着它。store 里不直接叫 ui
      // store，是为了不让这两个互相认识。
      //
      // 不怕刷屏：refresh 是边沿触发的（换项目、换集、干完一件事各一次），
      // 没有任何一处在轮询它。
      window.dispatchEvent(
        new CustomEvent('changji:error', {
          detail: `读不到这个项目：${err.message}`,
        }),
      )
      // 项目被删了或者路径不对，别把坏路径一直留着挡住后面的操作
      if (err.status === 400 || err.status === 404) {
        project.value = null
        flow.value = null
      }
    } finally {
      loading.value = false
    }
  }

  return {
    projectPath,
    episodeId,
    project,
    flow,
    loading,
    error,
    episodes,
    episode,
    characters,
    locations,
    done,
    counters,
    hasProject,
    selectProject,
    selectEpisode,
    clear,
    refresh,
  }
})
