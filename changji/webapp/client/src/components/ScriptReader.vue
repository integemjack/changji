<script setup>
/**
 * 剧本的阅读视图。
 *
 * 引擎那边的格式是定死的：对白一律「名字：台词」，动作单独成行。
 * 既然格式确定，就不该把一整块纯文本原样糊在页面上——审剧本的时候
 * 要看的是节奏：谁说了几句、中间隔了多少动作、哪一段全是描写没人说话。
 * 这些在等宽的纯文本里得一行行数，排成对白块之后一眼就看出来。
 */
import { computed } from 'vue'

const props = defineProps({
  text: { type: String, default: '' },
  // 一句台词大概几秒。用来在每句旁边标出粗估时长，
  // 判断篇幅长短比数字数直观。中文口语约每秒 5 个字。
  charsPerSecond: { type: Number, default: 5 },
})

/** 台词说话人的配色。同一个人从头到尾同一个颜色，扫一眼就认得出。 */
const HUES = [28, 200, 145, 320, 265, 95, 355, 175]

const parsed = computed(() => {
  const speakers = new Map()
  const blocks = []
  for (const raw of String(props.text).split('\n')) {
    const line = raw.trim()
    if (!line) continue
    // 全角冒号是引擎渲染时固定用的；半角一并认，手改的剧本常打成半角
    const at = line.search(/[：:]/)
    const name = at > 0 ? line.slice(0, at).trim() : ''
    // 名字那一段太长就不是说话人，是一句带冒号的描写
    const isDialogue = at > 0 && at <= 12 && !name.includes('，') && !name.includes('。')
    if (isDialogue) {
      if (!speakers.has(name)) speakers.set(name, HUES[speakers.size % HUES.length])
      blocks.push({ kind: 'dialogue', name, text: line.slice(at + 1).trim() })
    } else {
      blocks.push({ kind: 'action', text: line })
    }
  }
  return { blocks, speakers }
})

const stats = computed(() => {
  const lines = parsed.value.blocks.filter((b) => b.kind === 'dialogue')
  const chars = lines.reduce((a, b) => a + b.text.length, 0)
  return {
    dialogueLines: lines.length,
    actionLines: parsed.value.blocks.length - lines.length,
    chars,
    seconds: Math.round(chars / props.charsPerSecond),
  }
})

const hueOf = (name) => parsed.value.speakers.get(name) ?? 28
const secondsOf = (text) => Math.max(1, Math.round(text.length / props.charsPerSecond))
</script>

<template>
  <div class="reader">
    <div v-if="parsed.blocks.length" class="reader__bar">
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
        约 {{ stats.seconds }} 秒
      </span>
    </div>

    <div class="reader__body">
      <p v-if="!parsed.blocks.length" class="reader__empty muted small">
        还没有内容。
      </p>
      <template v-for="(b, i) in parsed.blocks" :key="i">
        <p v-if="b.kind === 'action'" class="action">{{ b.text }}</p>
        <div v-else class="line" :style="{ '--hue': hueOf(b.name) }">
          <span class="line__who">{{ b.name }}</span>
          <span class="line__text">{{ b.text }}</span>
          <span class="line__dur tiny numeric">{{ secondsOf(b.text) }}s</span>
        </div>
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
  max-height: 420px;
  overflow-y: auto;
  padding: var(--s4) var(--s5);
}
.reader__empty {
  text-align: center;
  padding: var(--s6) 0;
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
