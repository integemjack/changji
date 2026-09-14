/**
 * 项目库。
 *
 * **单独一个 store 而不是留在页面里**：项目列表现在有两个读它的地方——
 * 右边那条常驻的项目栏，和项目页自己。各读一份的话，建完项目右栏刷新了、
 * 页面没刷新，或者反过来；而两边显示的项目数不一样，用户会以为丢了一个。
 */

import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { api } from '@/api'

export const useProjects = defineStore('projects', () => {
  const items = ref([])
  const workspace = ref('')
  const loading = ref(false)
  const error = ref('')
  /** 有没有读过一次。第一次读之前不该显示「项目库还是空的」。 */
  const loaded = ref(false)

  const count = computed(() => items.value.length)

  function byPath(path) {
    if (!path) return null
    return items.value.find((p) => p.path === path) ?? null
  }

  /**
   * 重新列一遍项目库。
   *
   * **旧的那一趟不许盖新的。** 这个函数有好几个触发点（挂载、建完、删完、
   * 改完名、跑完一轮、换项目），叠起来很常见；而它没有参数，所以两趟回来
   * 的差别只有"新旧"——旧那份后落地，删掉的项目就会在栏里回来，而且不会
   * 自己消失，要等下一次触发。引擎那边读每个项目的 project.json 和
   * story.json（注释写着"可能有几百 KB"），慢到足以叠上。
   *
   * 没有参数可比，就用一个自增号：每趟开工时领一个，回来时不是最新的那个
   * 就直接扔掉。
   */
  let seq = 0

  async function load() {
    const mine = ++seq
    loading.value = true
    error.value = ''
    try {
      const data = await api.projects()
      if (mine !== seq) return
      workspace.value = data.workspace
      items.value = data.projects ?? []
    } catch (err) {
      if (mine !== seq) return
      error.value = err.message
    } finally {
      if (mine === seq) {
        loading.value = false
        loaded.value = true
      }
    }
  }

  return { items, workspace, loading, error, loaded, count, byPath, load }
})
