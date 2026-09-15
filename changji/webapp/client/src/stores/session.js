/**
 * 当前在做哪个项目的哪一集。
 *
 * 整个引导流程的上下文就这两个值，存 localStorage。
 * 关掉浏览器第二天回来，还能接着上次那一集往下走。
 */

import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { api } from '@/api'
import { dropLocal, readLocal, writeLocal } from '@/composables/local-storage'

const KEY_PROJECT = 'changji.project'
const KEY_EPISODE = 'changji.episode'

export const useSession = defineStore('session', () => {
  // **这两句在 App.vue 挂载之前跑**（App 的 setup 第一件事就是 useSession）。
  // 裸着用 localStorage 的话，一台把 cookie 设成「全部阻止」的浏览器上它
  // 会在属性访问那一下抛 SecurityError——位置在 ErrorBoundary 外面、
  // ToastStack 还不存在的时候，结果是一片白，界面上一个字都没有。
  // 见 composables/local-storage.js 开头那段。
  const projectPath = ref(readLocal(KEY_PROJECT) || '')
  const episodeId = ref(readLocal(KEY_EPISODE) || '')
  /** 上一趟 refresh 读砸了没有。**只给"引擎回来之后重来一趟"用**，界面不读。 */
  const failed = ref(false)

  const project = ref(null) // /api/project 的返回
  const flow = ref(null) // /bff/flow 的返回
  // **这儿原来还有 loading 和 error 两个 ref，2026-09-15 删了。**
  //
  // 两个都是只写不读：全仓没有一处读 `session.loading` 或 `session.error`
  // （每一页的转圈是自己那一趟的，报错走下面那条 changji:error）。
  // 而只写不读的状态不是没用，是**会骗人**——`loading` 那一个还正好是错的：
  // 它的 finally 判的是 `mine()`，而 refresh 自己在成功那一支里会
  // `selectEpisode(data.episodeId)` 把集号换掉（引擎回落到第一集这条路很常
  // 走），换完 mine() 当场变假，那句 `loading.value = false` 就不执行了。
  // 今天没人看得见，是因为 App 上那个 watch 会因为集号变了再 refresh 一趟、
  // 顺手把它抹平——一个只写不读的值，靠另一处的副作用碰巧擦干净。

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
    writeLocal(KEY_PROJECT, path)
    // 换项目时旧的集号一定对不上，先清掉，refresh 会挑第一集
    episodeId.value = ''
    dropLocal(KEY_EPISODE)
    project.value = null
    flow.value = null
  }

  function selectEpisode(id) {
    episodeId.value = id || ''
    if (id) writeLocal(KEY_EPISODE, id)
    else dropLocal(KEY_EPISODE)
  }

  function clear() {
    selectProject('')
    projectPath.value = ''
    dropLocal(KEY_PROJECT)
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
      failed.value = false
    } catch (err) {
      // 过期那一趟的报错也不能算数：上一部剧被删了回的 404，会把这一部
      // 的 project / flow 一起清掉，还弹一句莫名其妙的红字。
      if (!mine()) return
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
      // **记一笔，等引擎回来重来一趟。**
      //
      // 5xx / 连不上那条上面留着上一份数据（见上面那段），所以顶栏和侧边
      // 看着还是好的——但它是**上一次那一集**的那份：对勾、集号、每一集
      // 多少镜都停在那儿。而引擎重启是最常见的一种断，它两秒就回来了，
      // 这一份却要等到下一次换项目 / 换集 / 干完点带 refresh 的事才更新。
      // 谁来重来见 App.vue 里那句 useRetryWhenBack。
      failed.value = true
    }
  }

  return {
    projectPath,
    episodeId,
    failed,
    project,
    flow,
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
