# AmoledWatch landing page

The project's website: https://mikecheek.github.io/CustomOS-ESP32-S3-Amoled/

Vite + React + TypeScript + Tailwind CSS, no other runtime libraries. Every
watch screen on it is a screenshot the [host simulator](../tools/watch_sim)
renders from the firmware's own code (`docs/screenshots/`); `scripts/media.mjs`
copies them in at build time and, with ffmpeg, turns the GIFs into looping
MP4s and the power-on animation into the frames the first section scrubs
through as you scroll.

    npm install
    npm run dev       # http://localhost:5173/
    npm run build     # dist/

`src/lib.ts` has the scroll hooks (reveal on view, a section's scroll
progress for the sticky parts, count-ups); `src/components/Devices.tsx`
draws the watch and the phone in CSS; the sections are in `Story.tsx` and
`Sections.tsx`. Motion stops for "reduce motion".

Published by `.github/workflows/site.yml` on pushes to `main` that touch
`site/` or `docs/screenshots/` (pull requests only build it). Once:
Settings > Pages > Source: GitHub Actions.
