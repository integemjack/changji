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

const props = defineProps({
  text: { type: String, default: '' },
  // 一句台词大概几秒。**和引擎的预算一个数**（stages/script_prompt.inc.hpp
  // 的 kCharsPerSecond = 4.6）——两边不一样的话，这里说"约 13 秒"、
  // 后端说"偏短"，对不上。
  charsPerSecond: { type: Number, default: 4.6 },
  // 目标时长和对白字数预算。给了就在顶栏画出"够不够"。
  targetSeconds: { type: Number, default: 0 },
  budgetChars: { type: Number, default: 0 },
})

/** 台词说话人的配色。同一个人从头到尾同一个颜色，扫一眼就认得出。 */
const HUES = [28, 200, 145, 320, 265, 95, 355, 175]

// 段头：「【开场钩子 0–5 秒】」，或者没有秒数的「【开场钩子】」。
// 只认这四个名字，免得把「【字幕】三年后」「【倒计时 10 秒】」当成段头——
// 和引擎 parse_act_header 的判法一样。
const HEADER = /^【(开场钩子|冲突推进|情绪回报|集尾留扣)(?:\s+(\d+)[–-](\d+)\s*秒)?】$/

const parsed = computed(() => {
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
    const h = HEADER.exec(line)
    if (h) {
      cur = { label: h[1], from: Number(h[2] ?? 0), to: Number(h[3] ?? 0), blocks: [] }
      sections.push(cur)
      continue
    }
    // 全角冒号是引擎渲染时固定用的；半角一并认，手改的剧本常打成半角
    const at = line.search(/[：:]/)
    const name = at > 0 ? line.slice(0, at).trim() : ''
    // 名字那一段太长就不是说话人，是一句带冒号的描写
    const isDialogue = at > 0 && at <= 12 && !name.includes('，') && !name.includes('。')
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
  const chars = lines.reduce((a, b) => a + b.text.length, 0)
  return {
    dialogueLines: lines.length,
    actionLines: blocks.length - lines.length,
    chars,
    seconds: Math.round(chars / props.charsPerSecond),
  }
}

const stats = computed(() => statsOf(parsed.value.sections.flatMap((s) => s.blocks)))
const empty = computed(() => parsed.value.sections.every((s) => !s.blocks.length))

/** 够不够。阈值和后端 post_script_write 的 fit 一样：0.6 以下偏短，1.35 以上偏长。 */
const fit = computed(() => {
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
          <p v-if="b.kind === 'action'" class="action">{{ b.text }}</p>
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
