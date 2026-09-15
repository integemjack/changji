<script setup>
/**
 * 剧本的阅读视图。
 *
 * 引擎那边的格式是定死的：对白一律「名字：台词」，动作单独成行，段与段之间
 * 一行段头「【开场钩子 0–5 秒】」。既然格式确定，就不该把一整块纯文本原样
 * 糊在页面上——审剧本的时候要看的是节奏：这一段占几秒、里面谁说了几句、
 * 哪一段全是描写没人说话。排成段和对白块之后一眼就看出来。
 *
 * **2026-09-12 起按四段显示。** 行业里一集是四拍按秒排的（开场钩子 / 冲突
 * 推进 / 情绪回报 / 集尾留扣），时长是从剧本里长出来的。以前这里是一列平的
 * 拍子，60 秒的集写成 13 秒也看不出哪儿少了——现在每段底下有"几句、约几秒"，
 * 顶栏有"对白 x / 预算 字 · 约 x / 目标 秒"，缺在哪一段一眼能看见。
 */
import { computed } from 'vue'

import { ACT_LABELS } from '@/api/labels'

const props = defineProps({
  text: { type: String, default: '' },
  // 一句台词大概几秒。**和引擎的预算一个数**——真身是 `cpp/prompts.toml`
  // 的 `chars_per_second`（构建期生成到 stages/prompts.inc.hpp 的
  // `prompt::script::kCharsPerSecond`，今天 = 4.6）。两边不一样的话，
  // 这里说"约 13 秒"、后端说"偏短"，对不上。
  // 落后了当场红：api/speech-rate.test.js 直接读那个 toml 比。
  charsPerSecond: { type: Number, default: 4.6 },
  // 目标时长和对白字数预算。给了就在顶栏画出"够不够"。
  targetSeconds: { type: Number, default: 0 },
  budgetChars: { type: Number, default: 0 },
  /**
   * 现成的「够不够」裁决。给了就用它，不自己算。
   *
   * **两边的分子不是一回事。** 引擎数的是结构化的拍子（模型直接标了哪条是
   * 台词），而这一页是把渲染好的文本**再解析回来猜**——「冒号在前 12 个字
   * 以内」。碰上「字幕：三年后」「画外音：他没回头」这类带冒号的描写行，
   * 猜法会把它算成台词，对白字数就虚高，够不够的结论可能跟着翻面。
   *
   * 草稿那一屏两处都在（面板头上是引擎的、这条信息条上是自己算的），
   * 于是同一份稿子可能一个写「偏短」一个写「合适」。引擎那份是权威的，
   * 传进来就以它为准。
   */
  fit: { type: String, default: '' },
  /**
   * 现成的对白字数。给了就用它，不自己数。**和 `fit` 是一对**。
   *
   * 上面那段说清了两边的分子不是一回事：引擎数的是结构化拍子
   * （`ScriptDraft::dialogue_chars`，只累加 `kind == "dialogue"` 那几条的
   * `utf8_len`），而这一页是把渲染好的文本再解析回来猜。当初只把**裁决**
   * （fit）接过来了，**数字还是这边猜的**——于是草稿面板上会出现
   * 「对白 210 / 171 字」旁边挂着一颗绿的「合适」：数说超了，丸子说正好，
   * 并排站着自相矛盾。碰上「字幕：三年后」「画外音：他没回头」这类带冒号
   * 的描写行就会这样，猜法把它算成了台词。
   *
   * 引擎只给整篇这一个数，所以下面每一段那行「N 句 · 对白约 N 秒」照旧是
   * 这边猜的——那儿本来就写着「约」，而且分段的数引擎没有。
   */
  dialogueChars: { type: Number, default: 0 },
  /**
   * 现成的说话人名单。给了就**只认名单里的**，不再靠猜。
   *
   * 和 `fit`、`dialogueChars` 是同一族：真身在引擎的结构化拍子上
   * （`ScriptDraft::speakers()` 只收 `kind == "dialogue"` 那几条的
   * speaker），这一页是把渲染好的文本再解析回来猜——「冒号在前 12 个字
   * 以内、名字里没有逗号句号」。
   *
   * 猜错的那一类很具体：「字幕：三年后」「画外音：他没回头」这类**带冒号
   * 的描写行**会被当成台词。后果不只是数字虚高（那一半 dialogueChars 已经
   * 接过来了），还有**画面上的**：那一行会按台词排版、「字幕」会进说话人
   * 图例当成一个角色、每一段那句「N 句 · 对白约 N 秒」也跟着错。
   *
   * 名单是安全的：`render()` 写台词行用的就是 `b.speaker`，而 `speakers()`
   * 收的正是同一批——每一行 `名字：台词` 的名字必定在名单里，不会有真台词
   * 被这条判成描写。
   *
   * 引擎没给名单时（存下来的剧本那条路 `/api/script` 不回它）照旧靠猜。
   */
  speakers: { type: Array, default: () => [] },
})

/** 台词说话人的配色。同一个人从头到尾同一个颜色，扫一眼就认得出。 */
const HUES = [28, 200, 145, 320, 265, 95, 355, 175]

// 段头：「【开场钩子 0–5 秒】」，或者没有秒数的「【开场钩子】」。
//
// **名字只认 ACT_LABELS 那一份清单**，免得把「【字幕】三年后」
// 「【倒计时 10 秒】」当成段头。那份清单是引擎 prompts.toml 的
// `[script].act_labels` 抄过来的**全部十六个**——这儿原来写死了第 0 组
// 那四个（开场钩子/冲突推进/情绪回报/集尾留扣），而引擎会 `random_shape()`
// 现摇一组，五组里有四组根本不用这四个名字。那八成的剧本在这一页上
// 段头全被当成描写行，四段读数整个没了。
//
// **和引擎 parse_act_header 逐条对齐**（上一次修场次头那条时记下的两处漂移，
// 这次一起收掉）。段头分不对的后果不像场次头那么重（那条决定按场分镜怎么
// 切），但四段是这一页判断「这一段偏短」的依据，认错了那几句提示就没了：
//
//   · 引擎把括号里的内容 **strip 过**，所以「【 开场钩子 0–5 秒 】」它认，
//     而旧正则要求【后面紧跟名字、秒后面紧跟】，这一种整个不认；
//   · 反过来旧正则的 `\s*秒` 太松：引擎要求结尾是「 秒」（**秒前面必须有
//     一个空格**），「【开场钩子 0–5秒】」它当普通行，而这一页画成了段头。
//
// 九条用例对过：我们自己渲染的那种、半角短横、没有秒数、括号内带空格、
// 尾部空格、秒前无空格、【字幕】、【倒计时 10 秒】、场次头。
const HEADER = new RegExp(
  `^【\\s*(${ACT_LABELS.join('|')})(?:\\s+(\\d+)[–-](\\d+)\\s秒)?\\s*】$`,
)
// 场次头：「【第1场 · 夜 · 内 · 天台】」，序号后面那一截可有可无。
//
// **要和引擎 parse_scene_header 逐条对上**，因为这不只是显示——
// 按场分镜就是按它切的（stages/script.cpp 的 is_scene_header）。对不上的
// 后果是：人手打的一行，引擎当场次头在那儿切了一刀，而这一页把它画成
// 一句动作行，人看不见那一刀在哪。
//
// 原来这条差三处（注释却写着"判法一样"）：
//   · 引擎**允许只空一格**（「【第2场 夜 内 天台】」）——注释里就写着
//     "人手打的可能是「：」「，」「、」或者只空一格"。旧正则把分隔符
//     写成必需，这一种整个不认。
//   · 引擎收半角逗号 `,`，旧正则漏了；旧正则收 `/`，引擎不收。
//   · 引擎明确拒绝里面再出现【】，旧正则的 `(.*?)` 会把
//     「【第1场 · a】b【c】」吞成一条场次头。
//   · **首尾空白的定义不一样**（第四处，2026-09-15 补）。引擎第一步是
//     `strip_ascii`，而那个函数只去 **ASCII** 空白（util/text.cpp 的
//     `is_ascii_ws`）；JS 的 `.trim()` 连全角空格 U+3000 和 NBSP 一起去。
//     于是行首带一个全角空格的「【第1场 · 天台】」——中文段首缩进的习惯
//     写法，手打和粘贴都很容易带上——前端认、引擎不认：页面上画出一条
//     分场线，而分镜那一步根本不会在那儿切。方向和上面三条反过来，
//     后果是一样的：两边对不上，而人以为自己看见的就是要发生的。
const SCENE = /^【第(\d+)场\s*(?:[·：，、:,-]\s*)?([^【】]*?)\s*】$/

/** 只去 ASCII 空白，和引擎的 `strip_ascii` 一样。见上面第四条。 */
const asciiTrim = (s) => s.replace(/^[\t\n\v\f\r ]+|[\t\n\v\f\r ]+$/g, '')

const parsed = computed(() => {
  const known = new Set(props.speakers.map((x) => String(x).trim()).filter(Boolean))
  const speakers = new Map()
  const sections = []
  let cur = null
  // 段头之前的拍子（平的旧剧本、手写的）归到一个没名字的段里
  const section = () => {
    if (!cur) {
      cur = { label: '', from: 0, to: 0, blocks: [] }
      sections.push(cur)
    }
    return cur
  }
  for (const raw of String(props.text).split('\n')) {
    const line = raw.trim()
    if (!line) continue
    // 段头和场次头一样，按引擎那套空白规则判：`parse_act_header` 第一步
    // 也是 `strip_ascii`。见下面 SCENE 上那段第四条。
    const h = HEADER.exec(asciiTrim(raw))
    if (h) {
      cur = { label: h[1], from: Number(h[2] ?? 0), to: Number(h[3] ?? 0), blocks: [] }
      sections.push(cur)
      continue
    }
    // 场次头是内容不是段：留在段里，单独一种块，页面上画成一条分场线
    // 场次头按引擎那套空白规则判，不用上面 .trim() 过的 line
    const sc = SCENE.exec(asciiTrim(raw))
    if (sc) {
      section().blocks.push({ kind: 'scene', index: Number(sc[1]), text: (sc[2] ?? '').trim() })
      continue
    }
    // 全角冒号是引擎渲染时固定用的；半角一并认，手改的剧本常打成半角
    const at = line.search(/[：:]/)
    const name = at > 0 ? line.slice(0, at).trim() : ''
    // 有名单就按名单认，没有才退回猜。见 speakers 那条 prop。
    const isDialogue = known.size
      ? at > 0 && known.has(name)
      : at > 0 && at <= 12 && !name.includes('，') && !name.includes('。')
    if (isDialogue) {
      if (!speakers.has(name)) speakers.set(name, HUES[speakers.size % HUES.length])
      section().blocks.push({ kind: 'dialogue', name, text: line.slice(at + 1).trim() })
    } else {
      section().blocks.push({ kind: 'action', text: line })
    }
  }
  return { sections, speakers }
})

function statsOf(blocks) {
  const lines = blocks.filter((b) => b.kind === 'dialogue')
  // **按字数，不按 `.length`。** `.length` 数的是 UTF-16 码元，一个 emoji
  // 或者生僻字算两个；引擎那边是 `text::utf8_len`（码位），api/labels.js 的
  // `countScriptChars` 也是 `[...s]`。三处要同一个口径。
  const chars = lines.reduce((a, b) => a + [...b.text].length, 0)
  return {
    dialogueLines: lines.length,
    actionLines: blocks.length - lines.length,
    chars,
    seconds: Math.round(chars / props.charsPerSecond),
  }
}

const stats = computed(() => {
  const own = statsOf(parsed.value.sections.flatMap((s) => s.blocks))
  if (!props.dialogueChars) return own
  // 引擎给了就用引擎的，「约 x 秒」也跟着它走——那两个数是同一个分子算出来的，
  // 只换一个的话顶栏自己先打架。
  return {
    ...own,
    chars: props.dialogueChars,
    seconds: Math.round(props.dialogueChars / props.charsPerSecond),
  }
})
const empty = computed(() => parsed.value.sections.every((s) => !s.blocks.length))

/** 够不够。阈值和后端 post_script_write 的 fit 一样：0.6 以下偏短，1.35 以上偏长。 */
const fit = computed(() => {
  if (props.fit) return props.fit
  if (!props.budgetChars) return ''
  const r = stats.value.chars / props.budgetChars
  return r < 0.6 ? '偏短' : r > 1.35 ? '偏长' : '合适'
})

/**
 * 这一段的对白够不够它那几秒。
 *
 * 一段的时长里对白占六成（引擎的 kDialogueShare = 0.62），所以 28 秒的
 * 推进段该有约 17 秒的话；不到四成就标出来——缺内容的通常就是那一段。
 */
function thin(sec) {
  const span = sec.to - sec.from
  if (!span) return false
  return statsOf(sec.blocks).seconds < span * 0.62 * 0.4
}

const hueOf = (name) => parsed.value.speakers.get(name) ?? 28
const secondsOf = (text) => Math.max(1, Math.round(text.length / props.charsPerSecond))
</script>

<template>
  <div class="reader">
    <div v-if="!empty" class="reader__bar">
      <span
        v-for="[name, hue] in parsed.speakers"
        :key="name"
        class="who"
        :style="{ '--hue': hue }"
      >
        <i class="who__dot" />{{ name }}
      </span>
      <span class="spacer" />
      <span class="tiny dim numeric nowrap">
        {{ stats.dialogueLines }} 句台词 · {{ stats.actionLines }} 段描写 ·
        对白 {{ stats.chars }}<template v-if="budgetChars"> / {{ budgetChars }}</template> 字 ·
        约 {{ stats.seconds }}<template v-if="targetSeconds"> / {{ targetSeconds }}</template> 秒
      </span>
      <span v-if="fit" class="pill nowrap" :class="fit === '合适' ? 'pill--ok' : 'pill--warn'">
        {{ fit }}
      </span>
    </div>

    <div class="reader__body">
      <p v-if="empty" class="reader__empty muted small">
        还没有内容。
      </p>
      <template v-for="(sec, si) in parsed.sections" :key="si">
        <div v-if="sec.label" class="act" :class="{ 'act--thin': thin(sec) }">
          <span class="act__label">{{ sec.label }}</span>
          <span v-if="sec.to > sec.from" class="tiny dim numeric">{{ sec.from }}–{{ sec.to }} 秒</span>
          <span class="spacer" />
          <span class="tiny dim numeric nowrap">
            {{ statsOf(sec.blocks).dialogueLines }} 句 · 对白约 {{ statsOf(sec.blocks).seconds }} 秒<template v-if="thin(sec)">，这一段太薄</template>
          </span>
        </div>
        <template v-for="(b, i) in sec.blocks" :key="si + '-' + i">
          <p v-if="b.kind === 'scene'" class="scene">第{{ b.index }}场<template v-if="b.text"> · {{ b.text }}</template></p>
          <p v-else-if="b.kind === 'action'" class="action">{{ b.text }}</p>
          <div v-else class="line" :style="{ '--hue': hueOf(b.name) }">
            <span class="line__who">{{ b.name }}</span>
            <span class="line__text">{{ b.text }}</span>
            <span class="line__dur tiny numeric">{{ secondsOf(b.text) }}s</span>
          </div>
        </template>
      </template>
    </div>
  </div>
</template>

<style scoped>
.reader {
  border: 1px solid var(--line);
  border-radius: var(--r);
  background: var(--bg-sunken);
  overflow: hidden;
}

.reader__bar {
  display: flex;
  align-items: center;
  gap: var(--s2);
  flex-wrap: wrap;
  padding: var(--s2) var(--s4);
  border-bottom: 1px solid var(--line);
  background: var(--surface-2);
}
.who {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  font-size: var(--fs-sm);
  font-weight: 600;
  color: hsl(var(--hue) 70% 62%);
}
.who__dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: currentColor;
}

.reader__body {
  max-height: 60vh;
  overflow-y: auto;
  padding: var(--s4) var(--s5);
}
.reader__empty {
  text-align: center;
  padding: var(--s6) 0;
}

/* 段头：一条线加一个名字。太薄的段把名字标成警示色。 */
.act {
  display: flex;
  align-items: baseline;
  gap: var(--s2);
  flex-wrap: wrap;
  margin: var(--s4) 0 var(--s3);
  padding-top: var(--s3);
  border-top: 1px dashed var(--line-strong);
}
.act:first-child {
  margin-top: 0;
  padding-top: 0;
  border-top: 0;
}
.act__label {
  font-size: var(--fs-sm);
  font-weight: 700;
  color: var(--accent-text);
}
.act--thin .act__label {
  color: var(--warn);
}

.scene {
  margin: var(--s2) 0 var(--s1);
  padding-top: var(--s1);
  border-top: 1px dashed var(--line);
  font-size: var(--fs-sm);
  font-weight: 600;
  /* 没有 --fg-dim（是 --text-2 / --text-3）。作废之后这一行继承正文色，
     和台词一样重——而它是分场线，本该比内容轻一档。 */
  color: var(--text-2);
  letter-spacing: 0.02em;
}

.action {
  margin: 0 0 var(--s3);
  padding-left: var(--s3);
  border-left: 2px solid var(--line-strong);
  color: var(--text-3);
  font-size: var(--fs-base);
  line-height: 1.8;
}

.line {
  display: grid;
  grid-template-columns: 5.5em minmax(0, 1fr) auto;
  align-items: baseline;
  gap: var(--s3);
  margin-bottom: var(--s3);
  padding: var(--s2) var(--s3);
  border-radius: var(--r-sm);
  background: hsl(var(--hue) 60% 50% / 0.07);
  border-left: 2px solid hsl(var(--hue) 70% 55%);
}
.line__who {
  font-size: var(--fs-sm);
  font-weight: 700;
  color: hsl(var(--hue) 70% 62%);
  text-align: right;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.line__text {
  font-size: var(--fs-md);
  line-height: 1.75;
}
.line__dur {
  color: var(--text-3);
  opacity: 0;
  transition: opacity 0.14s var(--ease);
}
.line:hover .line__dur {
  opacity: 1;
}

@media (max-width: 640px) {
  .reader__body {
    padding: var(--s3);
  }
  .line {
    grid-template-columns: 1fr;
    gap: 2px;
  }
  .line__who {
    text-align: left;
  }
  .line__dur {
    display: none;
  }
}
</style>
