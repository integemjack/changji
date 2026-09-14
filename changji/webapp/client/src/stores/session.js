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
    /**
     * **这一趟是替谁问的。**
     *
     * 这个函数是全应用被叫得最勤的一个：App 上那个 watch（换项目、换集）、
     * `useAction` 里每个带 `refresh: true` 的动作、还有好几处手动调。两趟
     * 叠在一起太容易了，而 `/bff/flow` 恰恰是最慢的一条——它要读
     * project.json、story.json（引擎注释写着"可能有几百 KB"）、这一集的
     * 分镜和产物目录。
     *
     * 叠上之后，后落地的那一份会把**上一部剧**的 flow 装进来：侧栏的对勾、
     * 顶栏的集号、每一页读的 done 全是上一部的。更糟的是下面那句
     * `selectEpisode(data.episodeId)`——它会把集号切回上一部那一集，还写进
     * localStorage，于是新这一部的每一页都拿着一个不属于它的集号去问引擎。
     *
     * 判据要把集号也算进去：删一集那条路先 `selectEpisode('')` 再 refresh，
     * 靠引擎回落到第一集，那一趟的参数和上一趟不一样。
     */
    const want = `${projectPath.value}\u0000${episodeId.value}`
    const mine = () => want === `${projectPath.value}\u0000${episodeId.value}`
    loading.value = true
    error.value = ''
    try {
      const data = await api.flow(projectPath.value, episodeId.value)
      if (!mine()) return
      flow.value = data
      project.value = data.project
      // 引擎挑了哪一集就跟着它，省得前端自己再判一遍第一集是谁
      if (data.episodeId && data.episodeId !== episodeId.value) {
        selectEpisode(data.episodeId)
      }
      if (!data.episodeId) selectEpisode('')
    } catch (err) {
      // 过期那一趟的报错也不能算数：上一部剧被删了回的 404，会把这一部
      // 的 project / flow 一起清掉，还弹一句莫名其妙的红字。
      if (!mine()) return
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
      // 同理：过期那一趟的 finally 会在新那趟还读着的时候把转圈关掉
      if (mine()) loading.value = false
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
