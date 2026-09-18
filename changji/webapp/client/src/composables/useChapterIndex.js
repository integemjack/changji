import { computed, ref, watch } from 'vue'

import { api } from '@/api'
import { useSession } from '@/stores/session'

/**
 * 「这个人/这个地方出现在哪几章」。
 *
 * 用户 2026-09-16：「角色应该显示出现在哪几章里，和场景一样；章节只是梳理
 * 人物和场景的关系，这个页面不需要集的概念了。」
 *
 * 章节表里本来就记着每一章有谁、用到哪些地方（`chapter.characters` /
 * `chapter.locations`，是「读故事」那一步照着正文读出来的）。这里把那张表
 * **反过来**索引一遍：名字 → 出现在第几章。
 *
 * **靠名字对，不靠 id。** 章节里记的是正文里的称呼，资产库里存的是
 * char_id / location_id——两边唯一对得上的就是名字。
 */
export function useChapterIndex() {
  const session = useSession()
  const chapters = ref([])
  const loading = ref(false)

  async function load() {
    const path = session.projectPath
    if (!path) {
      chapters.value = []
      return
    }
    loading.value = true
    try {
      const got = await api.getStory(path)
      chapters.value = got?.story?.chapters ?? got?.chapters ?? []
    } catch {
      // 读不到就不显示这一栏。**不弹错**：这是张附加的表，
      // 故事还没写的项目本来就没有，为它弹一句红字只会挡住正事。
      chapters.value = []
    } finally {
      loading.value = false
    }
  }

  watch(() => session.projectPath, load, { immediate: true })

  /** 名字 → [{ no, title }]，no 从 1 数起，按章顺序。 */
  const index = computed(() => {
    const out = new Map()
    chapters.value.forEach((c, i) => {
      const entry = { no: i + 1, title: c.title ?? '' }
      for (const name of [...(c.characters ?? []), ...(c.locations ?? [])]) {
        const key = String(name ?? '').trim()
        if (!key) continue
        if (!out.has(key)) out.set(key, [])
        // 同一章里重复出现只算一次
        const rows = out.get(key)
        if (!rows.some((r) => r.no === entry.no)) rows.push(entry)
      }
    })
    return out
  })

  /** 这个名字出现在哪几章。没有就是空数组。 */
  function chaptersOf(name) {
    return index.value.get(String(name ?? '').trim()) ?? []
  }

  /** 「第 1、3、7 章」。一个都没有回空串，调用方据此不摆这一栏。 */
  function chapterLabel(name) {
    const rows = chaptersOf(name)
    if (!rows.length) return ''
    return '第 ' + rows.map((r) => r.no).join('、') + ' 章'
  }

  return { chapters, loading, chaptersOf, chapterLabel, reload: load }
}
