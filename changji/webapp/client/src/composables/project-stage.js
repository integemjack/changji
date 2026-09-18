/**
 * 这部电影到哪一步了。
 *
 * 项目页原来只有一条「出片百分比」的进度条，于是**刚建的空项目和故事写完
 * 还没分镜的项目长得一模一样**——都是 0%。而挑项目时真正想知道的就是
 * 「这部电影走到哪儿了」。
 *
 * 做成纯函数是为了能测：这一串判断错了不会报错，只会让卡片上写着一句
 * 不对的话，而那种错没人会发现。
 *
 * 判断顺序是**从后往前**：先看有没有成片，再往前推。反过来写的话，
 * 一个已经出完片、后来又加了一章大纲的项目，会被判成「还没展开正文」。
 */

/**
 * ⚠️ **rank 和 percent 是两个东西，别拿 percent 当排序键。**
 *
 * percent 是「这一条进度条画多长」，而它在 film 这一档是**档内比例**
 * （出了 3/12 章就是 25），别的档是跨档位置（outline 15、planned 45、
 * ready 60）。拿它排序的话，一部已经在发片的电影（25）会排到一部刚拉完
 * 章节计划、一镜没跑的电影（45）后面——「走得越远的越靠前」正好反过来。
 *
 * rank 是排序用的档次，越大越靠前。坏掉的排最前（出问题的东西正该最先
 * 看见，引擎那边把坏项目的 mtime 从 0.0 改回真值也是这个理由），空壳垫底。
 */
/** @typedef {{key: string, label: string, percent: number, rank: number, tone: string}} Stage */

/**
 * @param {object} p 项目列表里的一条
 * @returns {Stage}
 */
export function projectStage(p) {
  const outputs = p?.outputs ?? 0
  const shots = p?.shots ?? 0
  const done = p?.done_shots ?? 0
  const episodes = p?.episodes ?? 0
  const planned = p?.planned_episodes ?? 0
  const chapters = p?.chapters ?? 0
  const written = p?.written_chapters ?? 0

  if (p?.broken) {
    return { key: 'broken', label: '读不了', percent: 0, rank: 100, tone: 'bad' }
  }

  if (outputs > 0) {
    // **按章数算，不是 outputs>0 就 100%。** 原来一部十二章只出了一条
    // mp4 和全部出完，进度条上都是满格。分母用已落成的章数（引擎那边
    // 已经把预告排除在 episodes 和 outputs 之外了）。
    const total = Math.max(episodes, outputs)
    const pct = total > 0 ? Math.round((outputs / total) * 100) : 100
    return {
      key: 'film',
      label: total > outputs ? `${outputs}/${total} 章已出片` : `${outputs} 章已出片`,
      percent: pct,
      rank: 90,
      tone: pct >= 100 ? 'ok' : 'accent',
    }
  }

  if (shots > 0) {
    // 镜头全出完但还没装配，是个真实存在的中间态：装配要 ffmpeg，
    // 缺它的机器会停在这儿。写成「100% 已出片」会让人以为完事了。
    if (done >= shots) {
      return { key: 'assemble', label: '镜头出完了，还没装配', percent: 95, rank: 80, tone: 'warn' }
    }
    // **一镜都没跑不是「出片 0%」。** 后者读起来像在进行中，而实际是
    // 分镜出好了、还没按开始。这两种状态下用户要做的事不一样。
    if (done === 0) {
      return { key: 'ready', label: `${shots} 镜待出片`, percent: 60, rank: 60, tone: 'accent' }
    }
    return {
      key: 'shooting',
      label: `出片 ${Math.round((done / shots) * 100)}%`,
      percent: Math.round((done / shots) * 90),
      rank: 70,
      tone: 'accent',
    }
  }

  if (episodes > 0) {
    // 章节计划算了几章、真落成了几章，是两个数。「10 章里落成 3 章」比
    // 「3 章，还没分镜」说得清楚——planned_episodes 引擎一直在算、
    // 2026-09-14 之前前端一处都没读。
    const label =
      planned > episodes
        ? `${planned} 章里落成 ${episodes} 章，还没分镜`
        : `${episodes} 章，还没分镜`
    return { key: 'planned', label, percent: 45, rank: 50, tone: 'accent' }
  }

  // 下面三档全在故事层里。原来这一段是一片空白——都显示 0%。
  if (written > 0) {
    return {
      key: 'written',
      label: `正文 ${written}/${chapters} 章，还没落成章节`,
      percent: 30,
      rank: 40,
      tone: 'accent',
    }
  }
  if (chapters > 0) {
    return { key: 'outline', label: `${chapters} 章大纲，还没展开正文`, percent: 15, rank: 30, tone: 'accent' }
  }
  // **「故事坏了」和「还没写故事」不是一回事。** 引擎读 story.json 失败时
  // 把 chapters 留 0 就过去了（那是对的：一个坏文件不该让整个项目库列不
  // 出来），但顺手把这两种压成了同一句话——一个正文全写完、只是 JSON 崩了
  // 的项目，在栏里长得和 smoke-tmp 一模一样，顺手删掉的正是投入最多的那个。
  //
  // 这一档排在 empty 之前、broken 之后：broken 是整个项目打不开，这个只是
  // 故事那一份坏了，别的（分镜、成片）照样能用，所以不能复用 broken。
  if (p?.story_broken) {
    return { key: 'story-broken', label: '故事文件坏了', percent: 0, rank: 99, tone: 'bad' }
  }
  // 资产库坏了同理，和 story_broken 一个待遇。**排在 empty 之前**：
  // 一个定完妆的项目，assets.json 形状歪了之后在这条列表上原来显示成
  // 「还没写故事」——和空壳一模一样，而空壳正是顺手删掉的那一类。
  if (p?.assets_broken) {
    return { key: 'assets-broken', label: '设定文件坏了', percent: 0, rank: 98, tone: 'bad' }
  }
  return { key: 'empty', label: '还没写故事', percent: 0, rank: 0, tone: 'dim' }
}
