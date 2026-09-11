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

  async function load() {
    loading.value = true
    error.value = ''
    try {
      const data = await api.projects()
      workspace.value = data.workspace
      items.value = data.projects ?? []
    } catch (err) {
      error.value = err.message
    } finally {
      loading.value = false
      loaded.value = true
    }
  }

  return { items, workspace, loading, error, loaded, count, byPath, load }
})
