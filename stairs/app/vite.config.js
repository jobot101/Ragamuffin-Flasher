import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'

// One self-contained index.html (JS + CSS inlined) so the board can serve it on its own WiFi with no internet.
// `npm run dev` talks to a real board: BOARD=http://192.168.1.250 npm run dev (default hall-stairs.local).
const board = process.env.BOARD || 'http://hall-stairs.local'
const api = ['/state', '/set', '/cfg', '/scan', '/wifi', '/mqtt', '/update']

export default defineConfig({
  plugins: [react(), viteSingleFile()],
  build: { target: 'es2020' },
  server: { proxy: Object.fromEntries(api.map(p => [p, board])) },
})
