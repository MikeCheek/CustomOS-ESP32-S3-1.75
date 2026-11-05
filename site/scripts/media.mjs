// Gets the site's pictures ready from the watch's own screenshots
// (../docs/screenshots, rendered by tools/watch_sim from the real UI code),
// so nothing is copied into the repo twice:
//   public/shots/   the PNGs and GIFs as they are
//   public/media/   each GIF as a short looping MP4 (smaller, smoother)
//   public/seq/     the power-on animation frame by frame, for the scroll-scrubbed hero
// Without ffmpeg the videos and frames are skipped and the page falls back to the GIFs.
import { cpSync, mkdirSync, readdirSync, rmSync, existsSync } from 'node:fs'
import { execFileSync } from 'node:child_process'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const src = join(here, '..', '..', 'docs', 'screenshots')
const pub = join(here, '..', 'public')
const shots = join(pub, 'shots'), media = join(pub, 'media'), seq = join(pub, 'seq')
for (const d of [shots, media, seq]) { rmSync(d, { recursive: true, force: true }); mkdirSync(d, { recursive: true }) }
cpSync(src, shots, { recursive: true })

let ffmpeg = true
try { execFileSync('ffmpeg', ['-version'], { stdio: 'ignore' }) } catch { ffmpeg = false }
if (!ffmpeg) { console.log('media: no ffmpeg - GIFs only'); process.exit(0) }

const run = (args) => execFileSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', ...args])
const gifs = readdirSync(src).filter((f) => f.endsWith('.gif'))
for (const g of gifs) {
  const name = g.replace(/\.gif$/, '')
  // black behind the transparent corners, 480 px, H.264 for every browser
  run(['-i', join(src, g), '-filter_complex',
       'color=black:s=480x480[bg];[0:v]scale=480:480:flags=lanczos[fg];[bg][fg]overlay=shortest=1,format=yuv420p',
       '-movflags', '+faststart', '-c:v', 'libx264', '-crf', '24', '-preset', 'slow', join(media, name + '.mp4')])
}
// the power-on animation as frames: the hero scrubs through them with the scroll
if (existsSync(join(src, 'anim_boot.gif'))) {
  run(['-i', join(src, 'anim_boot.gif'), '-vf', 'scale=480:480:flags=lanczos', '-c:v', 'libwebp', '-quality', '82',
       join(seq, 'boot_%03d.webp')])
}
console.log(`media: ${gifs.length} videos, ${readdirSync(seq).length} frames`)
