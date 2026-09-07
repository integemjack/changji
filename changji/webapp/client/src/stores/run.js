/**
 * 流水线运行状态。
 *
 * 轮询而不是 WebSocket：这一层要显示的东西每秒变一次就够了，
 * 少一层连接管理就少一类掉线问题。引擎那边也是这个思路。
 */

import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { api } from '@/api'

const STAGE_LABELS = {
  audio: '配音',
  frames: '首帧',
  draft: '草稿档',
  final: '成片档',
  lipsync: '口型',
  gate: '闸门',
  assemble: '装配',
  done: '完成',
}

export const useRun = defineStore('run', () => {
  const state = ref(null)
  const polling = ref(false)
  let timer = null
  let missCount = 0

  const running = computed(() => Boolean(state.value?.running))
  const percent = computed(() => {
    const s = state.value
    if (!s || !s.total) return 0
    return Math.min(100, Math.round((s.current / s.total) * 100))
  })
  const stageLabel = computed(() => {
    const stage = state.value?.stage
    return stage ? (STAGE_LABELS[stage] ?? stage) : ''
  })
  const events = computed(() => state.value?.events ?? [])

  async function poll() {
    try {
      state.value = await api.runStatus()
      missCount = 0
    } catch {
      // 引擎重启时会连着失败几次。立刻报错太吵，连丢三次再说。
      missCount += 1
      if (missCount >= 3) stop()
    }
  }

  function start(intervalMs = 1200) {
    if (polling.value) return
    polling.value = true
    poll()
    timer = setInterval(poll, intervalMs)
  }

  function stop() {
    polling.value = false
    if (timer) clearInterval(timer)
    timer = null
  }

  return { state, running, percent, stageLabel, events, polling, poll, start, stop }
})

/** 写整季剧本 / 批量出分镜的进度。跟流水线是两条独立的线。 */
export const useWriter = defineStore('writer', () => {
  const state = ref(null)
  const polling = ref(false)
  let timer = null

  const running = computed(() => Boolean(state.value?.running))
  const percent = computed(() => {
    const s = state.value
    if (!s || !s.total) return 0
    return Math.min(100, Math.round((s.done / s.total) * 100))
  })

  async function poll() {
    try {
      state.value = await api.seriesStatus()
      if (!state.value.running) stop()
    } catch {
      stop()
    }
  }

  function start(intervalMs = 1500) {
    if (polling.value) return
    polling.value = true
    poll()
    timer = setInterval(poll, intervalMs)
  }

  function stop() {
    polling.value = false
    if (timer) clearInterval(timer)
    timer = null
  }

  return { state, running, percent, polling, poll, start, stop }
})
