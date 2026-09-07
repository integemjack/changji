/**
 * 这份配置只为一件事存在：把「用了没导入的东西」在构建前拦下来。
 *
 * 起因是一次真事故——SettingsView 里用了 watch 但没从 vue 导入。
 * Vite 编译得过，页面打开是一片空白，控制台里躺着一句
 * ReferenceError: watch is not defined。整页白屏而没有任何提示，
 * 靠肉眼是发现不了的，得正好打开那一页才知道。
 *
 * 所以规则挑得很克制：只留 no-undef 这一类真会让页面崩掉的，
 * 不做代码风格上的规训。风格靠看，崩溃靠工具。
 */

import js from '@eslint/js'
import vue from 'eslint-plugin-vue'
import globals from 'globals'

export default [
  { ignores: ['dist/**', 'node_modules/**'] },
  js.configs.recommended,
  ...vue.configs['flat/essential'],
  {
    languageOptions: {
      ecmaVersion: 2023,
      sourceType: 'module',
      globals: { ...globals.browser },
    },
    rules: {
      // 就是它。用了没导入 / 拼错了变量名，在这里断掉。
      'no-undef': 'error',
      'no-unused-vars': ['warn', { argsIgnorePattern: '^_' }],
      // 下面这些是风格，不拦
      'vue/multi-word-component-names': 'off',
      'no-empty': ['error', { allowEmptyCatch: true }],
    },
  },
]
