import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// Relative paths: the page works wherever it's served - GitHub Pages' /<repo>/
// (whatever the repo is called), a fork, or a local preview.
export default defineConfig({
  base: './',
  plugins: [react(), tailwindcss()],
})
