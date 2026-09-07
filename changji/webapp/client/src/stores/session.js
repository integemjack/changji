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
