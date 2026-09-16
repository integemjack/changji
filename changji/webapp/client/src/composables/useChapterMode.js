import { ref } from 'vue'

/**
 * **章模式现在是唯一的模式。**
 *
 * 用户 2026-09-16：设定里的分集应该就是剧本，「这一集」应该叫「这一章」；
 * 同一天又定了只留章模式、把集模式删掉。引擎那边 `[assembly].episode_s`
 * 也不再是开关了，只是"一集多长"（装配时按它切），老配置里的 0 在读取时
 * 抬到默认值。
 *
 * 所以这儿原来那趟 `/bff/settings/overview` 没了：
 *
 *   · 它本来就只用来回答"开没开"，而答案现在恒为真；
 *   · 而且它**会答错**——引擎重启那几秒请求被拒，catch 里按老叫法兜底，
 *     于是标签页写「这一集 · 场记」而屏幕上写「这一章」。2026-09-16 实见。
 *
 * 留着这个 ref 和 chapterWord 是为了不动那一百多处调用点：等「这一集」
 * 那些文案统一扫完再把它们一起删掉。
 */
const chapter = ref(true)

export function useChapterMode() {
  return { chapter }
}

/** 步骤条、页签上的名字：「这一集」说成「这一章」，「分集」说成「章节」。 */
export function chapterWord(title, isChapter) {
  if (!isChapter) return title
  if (title === '这一集') return '这一章'
  if (title === '分集') return '章节'
  return title
}
