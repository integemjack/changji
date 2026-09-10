<script setup>
/**
 * 图标。
 *
 * 自己画，不引图标库。这里前后只用到十几个，为此拉一个几百 KB 的包
 * 进来不划算，而且图标库的线宽和这套界面的线宽经常对不上。
 */
defineProps({
  name: { type: String, required: true },
  size: { type: [Number, String], default: 16 },
})

const PATHS = {
  folder: 'M3 6a2 2 0 0 1 2-2h3.6l1.7 2H19a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z',
  script: 'M7 3h9l4 4v14H7zM16 3v4h4M10 12h7M10 16h7M4 6v15h12',
  user: 'M12 12a4 4 0 1 0 0-8 4 4 0 0 0 0 8M4.5 20a7.5 7.5 0 0 1 15 0',
  scene: 'M3 5h18v14H3zM3 15l5-5 4 4 3-3 6 6',
  board: 'M3 4h18v16H3zM3 10h18M3 15h18M9 4v16M15 4v16',
  gear: 'M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6M19.4 15a1.6 1.6 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.6 1.6 0 0 0-1.8-.3 1.6 1.6 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1A1.6 1.6 0 0 0 9 19.4a1.6 1.6 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.6 1.6 0 0 0 .3-1.8 1.6 1.6 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1A1.6 1.6 0 0 0 4.6 9a1.6 1.6 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.6 1.6 0 0 0 1.8.3H9a1.6 1.6 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.6 1.6 0 0 0 1 1.5 1.6 1.6 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.6 1.6 0 0 0-.3 1.8V9a1.6 1.6 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.6 1.6 0 0 0-1.5 1',
  film: 'M3 4h18v16H3zM7 4v16M17 4v16M3 9h4M3 15h4M17 9h4M17 15h4',
  upload: 'M12 16V4M8 8l4-4 4 4M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3',
  check: 'M4 12.5 9.5 18 20 6.5',
  arrowRight: 'M5 12h14M13 5l7 7-7 7',
  arrowLeft: 'M19 12H5M11 19l-7-7 7-7',
  sparkle: 'M12 3l1.9 5.6L19.5 10.5l-5.6 1.9L12 18l-1.9-5.6L4.5 10.5l5.6-1.9zM19 3.5v3M17.5 5h3',
  refresh: 'M20 11a8 8 0 1 0-.8 4.5M20 5v6h-6',
  plus: 'M12 5v14M5 12h14',
  trash: 'M4 7h16M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2M6 7l1 13a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1l1-13',
  close: 'M6 6l12 12M18 6L6 18',
  menu: 'M4 7h16M4 12h16M4 17h16',
  play: 'M7 4.5 19 12 7 19.5z',
  stop: 'M6 6h12v12H6z',
  pause: 'M9 5v14M15 5v14',
  warn: 'M12 4 2.5 20.5h19zM12 10v5M12 18h.01',
  info: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18M12 11v6M12 7.5h.01',
  link: 'M10 13a4 4 0 0 0 5.7.3l3-3a4 4 0 1 0-5.7-5.7L11.5 6M14 11a4 4 0 0 0-5.7-.3l-3 3a4 4 0 1 0 5.7 5.7L12.5 18',
  image: 'M3 5h18v14H3zM3 16l5-5 4 4 3-2 6 5M15.5 9h.01',
  sun: 'M12 17a5 5 0 1 0 0-10 5 5 0 0 0 0 10M12 1.5v2M12 20.5v2M3.9 3.9l1.4 1.4M18.7 18.7l1.4 1.4M1.5 12h2M20.5 12h2M3.9 20.1l1.4-1.4M18.7 5.3l1.4-1.4',
  moon: 'M20 14.5A8.5 8.5 0 0 1 9.5 4a8.5 8.5 0 1 0 10.5 10.5',
  wand: 'M4 20 16 8M14 4l1.2 2.8L18 8l-2.8 1.2L14 12l-1.2-2.8L10 8l2.8-1.2zM19.5 13l.8 1.7 1.7.8-1.7.8-.8 1.7-.8-1.7-1.7-.8 1.7-.8z',
}
</script>

<template>
  <svg
    class="icon"
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="1.6"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
  >
    <path :d="PATHS[name] || PATHS.info" />
  </svg>
</template>

<style scoped>
.icon {
  flex: none;
  display: block;
}
</style>
