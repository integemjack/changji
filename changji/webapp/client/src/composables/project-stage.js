/**
 * 这部剧到哪一步了。
 *
 * 项目页原来只有一条「出片百分比」的进度条，于是**刚建的空项目和故事写完
 * 还没分镜的项目长得一模一样**——都是 0%。而挑项目时真正想知道的就是
 * 「这部剧走到哪儿了」。
 *
 * 做成纯函数是为了能测：这一串判断错了不会报错，只会让卡片上写着一句
 * 不对的话，而那种错没人会发现。
 *
 * 判断顺序是**从后往前**：先看有没有成片，再往前推。反过来写的话，
 * 一个已经出完片、后来又加了一章大纲的项目，会被判成「还没展开正文」。
 */

/** @typedef {{key: string, label: string, percent: number, tone: string}} Stage */

/**
 * @param {object} p 项目列表里的一条
 * @returns {Stage}
 */
export function projectStage(p) {
  const outputs = p?.outputs ?? 0
  const shots = p?.shots ?? 0
  const done = p?.done_shots ?? 0
  const episodes = p?.episodes ?? 0
  const chapters = p?.chapters ?? 0
  const written = p?.written_chapters ?? 0

  if (p?.broken) {
    return { key: 'broken', label: '读不了', percent: 0, tone: 'bad' }
  }

  if (outputs > 0) {
    return {
      key: 'film',
      label: `${outputs} 个成片`,
      percent: 100,
      tone: 'ok',
    }
  }

  if (shots > 0) {
    // 镜头全出完但还没装配，是个真实存在的中间态：装配要 ffmpeg，
    // 缺它的机器会停在这儿。写成「100% 已出片」会让人以为完事了。
    if (done >= shots) {
      return { key: 'assemble', label: '镜头出完了，还没装配', percent: 95, tone: 'warn' }
    }
    return {
      key: 'shooting',
      label: `出片 ${Math.round((done / shots) * 100)}%`,
      percent: Math.round((done / shots) * 90),
      tone: 'accent',
    }
  }

  if (episodes > 0) {
    return { key: 'planned', label: `${episodes} 集，还没分镜`, percent: 45, tone: 'accent' }
  }

  // 下面三档全在故事层里。原来这一段是一片空白——都显示 0%。
  if (written > 0) {
    return {
      key: 'written',
      label: `正文 ${written}/${chapters} 章，还没落成剧集`,
      percent: 30,
      tone: 'accent',
    }
  }
  if (chapters > 0) {
    return { key: 'outline', label: `${chapters} 章大纲，还没展开正文`, percent: 15, tone: 'accent' }
  }
  return { key: 'empty', label: '还没写故事', percent: 0, tone: 'dim' }
}
