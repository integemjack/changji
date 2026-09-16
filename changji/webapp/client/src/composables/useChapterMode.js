import { ref } from 'vue'

import { api } from '@/api'

/**
 * 章模式开没开（[assembly].episode_s > 0）。
 *
 * 用户 2026-09-16 的判词：设定里的分集应该就是剧本，「这一集」应该叫
 * 「这一章」——一章按内容写完、拍完，最后按固定的每集时长切成几集。
 * 引擎那边四处已经按这个数切换了；界面上先把最显眼的两个叫法跟上：
 * 步骤条里的「这一集」和设定页的「分集」。别的一百多处「这一集」文案
 * 等这条路跑顺了再统一改，现在改一半比不改更乱。
 *
 * 读一次就够：这个数改了要在设置页保存，保存那一页自己会刷新。
 */
const chapter = ref(false)
let loaded = false

export function useChapterMode() {
  if (!loaded) {
    loaded = true
    api
      .settingsOverview()
      .then((d) => {
        chapter.value = Number(d?.settings?.assembly?.episode_s ?? 0) > 0
      })
      .catch(() => {
        // 读不到就按老叫法。这是个称呼，不值得弹错
      })
  }
  return { chapter }
}

/** 步骤条、页签上的名字：章模式把「这一集」说成「这一章」，「分集」说成「章节」。 */
export function chapterWord(title, isChapter) {
  if (!isChapter) return title
  if (title === '这一集') return '这一章'
  if (title === '分集') return '章节'
  return title
}
