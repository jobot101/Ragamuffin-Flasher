import { defineConfig } from 'vite'

// served from https://jobot101.github.io/ragamuffin-flasher/ — relative base so it works under any path
export default defineConfig({ base: './', build: { outDir: 'dist', target: 'es2022' } })
