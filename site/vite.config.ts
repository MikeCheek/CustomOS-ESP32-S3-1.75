import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// GitHub Pages serves the site at /<repo>/
export default defineConfig({
  base: process.env.SITE_BASE ?? '/CustomOS-ESP32-S3-1.75/',
  plugins: [react(), tailwindcss()],
})
