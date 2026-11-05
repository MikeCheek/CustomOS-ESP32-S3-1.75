import { useEffect, useRef } from 'react'
import { Loop, Shot, Watch } from './Devices'
import { asset, reducedMotion, seg, useSectionProgress } from '../lib'

const REPO = 'https://github.com/MikeCheek/CustomOS-ESP32-S3-Amoled'

export function Nav() {
  const links: [string, string][] = [['Watch', '#watch'], ['Health', '#health'], ['AI', '#ai'], ['Games', '#games'], ['Power', '#power'], ['App', '#app']]
  return (
    <nav className="fixed inset-x-0 top-0 z-50 border-b border-white/5 bg-black/60 backdrop-blur-xl">
      <div className="mx-auto flex h-12 max-w-6xl items-center justify-between px-5 text-sm">
        <a href="#top" className="flex items-center gap-2 font-semibold tracking-tight">
          <span className="inline-block h-4 w-4 rounded-full border-[3px] border-accent" /> AmoledWatch
        </a>
        <div className="hidden gap-6 text-ink md:flex">
          {links.map(([t, h]) => <a key={h} href={h} className="transition hover:text-white">{t}</a>)}
        </div>
        <a href={`${REPO}/releases/latest`} className="rounded-full bg-white px-3.5 py-1 font-medium text-black transition hover:bg-white/85">Get it</a>
      </div>
    </nav>
  )
}

/** The opening: the watch, big, turning a little as you scroll away. */
export function Hero() {
  const [ref, p] = useSectionProgress<HTMLElement>()
  return (
    <section id="top" ref={ref} className="relative h-[170vh]">
      <div className="sticky top-0 flex h-screen flex-col items-center overflow-hidden px-5 pt-[max(4.5rem,9vh)]">
        <div className="pointer-events-none absolute inset-0" style={{ background: 'radial-gradient(60% 50% at 50% 55%, rgba(124,108,255,.28), transparent 70%), radial-gradient(40% 30% at 70% 30%, rgba(58,160,255,.15), transparent 70%)' }} />
        <p className="reveal in mb-3 text-sm font-medium uppercase tracking-[0.3em] text-accent">ESP32-S3 · 466 × 466 AMOLED</p>
        <h1 className="text-center text-5xl font-semibold leading-[1.02] tracking-tight sm:text-7xl md:text-[5.5rem]" style={{ opacity: 1 - seg(p, 0.35, 0.7), transform: `translateY(${-p * 60}px)` }}>
          <span className="text-gradient">Your watch.</span><br />Your OS.
        </h1>
        <p className="mt-5 max-w-xl text-center text-lg text-ink" style={{ opacity: 1 - seg(p, 0.25, 0.55) }}>
          A full smartwatch OS, written from scratch for a round AMOLED - with a companion app that runs its AI on your phone, not in the cloud.
        </p>
        <div className="mt-6 flex gap-3" style={{ opacity: 1 - seg(p, 0.2, 0.5) }}>
          <a href={`${REPO}/releases/latest`} className="rounded-full bg-accent px-6 py-2.5 font-medium transition hover:brightness-110">Download</a>
          <a href={REPO} className="rounded-full border border-white/20 px-6 py-2.5 font-medium transition hover:bg-white/10">GitHub</a>
        </div>
        <div className="glow mt-8" style={{ transform: `perspective(1200px) rotateX(${p * 28}deg) scale(${1 - p * 0.25}) translateY(${p * -40}px)` }}>
          <Watch size={230}><Shot name="watchface.png" alt="The default watch face with complications" /></Watch>
        </div>
      </div>
    </section>
  )
}

/** Apple-style: the power-on animation scrubbed frame by frame with the scroll. */
export function BootScrub() {
  const [ref, p] = useSectionProgress<HTMLElement>()
  const canvas = useRef<HTMLCanvasElement>(null)
  const frames = useRef<HTMLImageElement[]>([])
  const N = 55
  useEffect(() => {
    frames.current = Array.from({ length: N }, (_, i) => {
      const im = new Image()
      im.src = asset(`seq/boot_${String(i + 1).padStart(3, '0')}.webp`)
      return im
    })
  }, [])
  useEffect(() => {
    const c = canvas.current?.getContext('2d')
    if (!c) return
    const i = Math.min(N - 1, Math.floor(seg(p, 0.05, 0.75) * (N - 1)))
    const draw = (k: number) => {
      const im = frames.current[k]
      if (im?.complete && im.naturalWidth) { c.clearRect(0, 0, 480, 480); c.drawImage(im, 0, 0, 480, 480); return true }
      return false
    }
    if (!draw(i)) frames.current[i]?.addEventListener('load', () => draw(i), { once: true })
  }, [p])
  const lines = [
    ['No LVGL. No borrowed UI kit.', 'Every screen, transition and 3D effect is drawn by its own code, on Arduino_GFX.'],
    ['Particles, then the face.', 'A power-on sequence in software, tilting lock screens, a CRT switch-off.'],
    ['Built for the circle.', 'Lists that follow the round edge, arcs and rings instead of boxes.'],
  ]
  return (
    <section ref={ref} className="relative h-[300vh]">
      <div className="sticky top-0 flex h-screen items-center justify-center overflow-hidden px-5">
        <div className="grid w-full max-w-6xl items-center gap-10 md:grid-cols-2">
          <div className="relative order-2 h-56 md:order-1">
            {lines.map(([h, t], k) => {
              const a = seg(p, 0.1 + k * 0.25, 0.22 + k * 0.25), b = 1 - seg(p, 0.3 + k * 0.25, 0.38 + k * 0.25)
              const o = k === lines.length - 1 ? a : Math.min(a, b)
              return (
                <div key={h} className="absolute inset-0" style={{ opacity: o, transform: `translateY(${(1 - o) * 30}px)` }}>
                  <h2 className="text-4xl font-semibold tracking-tight md:text-5xl">{h}</h2>
                  <p className="mt-4 max-w-md text-lg text-ink">{t}</p>
                </div>
              )
            })}
          </div>
          <div className="order-1 flex justify-center md:order-2">
            <Watch size={300} strap={false}>
              {reducedMotion() ? <Loop name="anim_boot" alt="The power-on animation" /> : <canvas ref={canvas} width={480} height={480} className="h-full w-full" />}
            </Watch>
          </div>
        </div>
      </div>
    </section>
  )
}

/** Faces and screens, sliding sideways as you scroll down. */
export function FaceRail() {
  const [ref, p] = useSectionProgress<HTMLElement>()
  const items: [string, string][] = [
    ['watchface.png', 'Default face, with complications'], ['watchface_minimal.png', 'Minimal'], ['aod.png', 'Always-on display'],
    ['face_switcher.png', 'Hold to switch faces'], ['menu.png', 'One honeycomb of apps'], ['smart_stack.png', 'Smart Stack'],
    ['quick_panel.png', 'Quick panel'], ['tile_music.png', 'Tiles'], ['menu_list.png', 'Or an A-Z list'],
  ]
  return (
    <section id="watch" ref={ref} className="relative h-[260vh]">
      <div className="sticky top-0 flex h-screen flex-col justify-center overflow-hidden">
        <div className="mx-auto mb-10 w-full max-w-6xl px-5">
          <p className="text-sm font-medium uppercase tracking-[0.3em] text-accent">The watch</p>
          <h2 className="mt-2 text-4xl font-semibold tracking-tight md:text-6xl">Faces you design.<br /><span className="text-ink">Gestures you choose.</span></h2>
        </div>
        <div className="flex gap-10 pl-[8vw] will-change-transform" style={{ transform: `translateX(${-p * 62}%)` }}>
          {items.map(([n, t], i) => (
            <figure key={n} className="shrink-0 text-center" style={{ transform: `translateY(${Math.sin(p * 6 + i) * 14}px)` }}>
              <Watch size={210} strap={false}><Shot name={n} alt={t} /></Watch>
              <figcaption className="mt-4 text-sm text-ink">{t}</figcaption>
            </figure>
          ))}
        </div>
      </div>
    </section>
  )
}
