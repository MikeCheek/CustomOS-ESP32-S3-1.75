import { useEffect, useRef, useState, type RefObject } from 'react'

/** A file the build put in public/ (shots/, media/, seq/), at the site's base path. */
export const asset = (p: string) => `${import.meta.env.BASE_URL}${p}`

export const reducedMotion = () =>
  typeof window !== 'undefined' && window.matchMedia('(prefers-reduced-motion: reduce)').matches

/** True once the element has scrolled into view (it stays true). */
export function useInView<T extends Element>(threshold = 0.2): [RefObject<T | null>, boolean] {
  const ref = useRef<T>(null)
  const [seen, setSeen] = useState(false)
  useEffect(() => {
    const el = ref.current
    if (!el || seen) return
    const io = new IntersectionObserver(([e]) => e.isIntersecting && setSeen(true), { threshold })
    io.observe(el)
    return () => io.disconnect()
  }, [seen, threshold])
  return [ref, seen]
}

/**
 * How far the page has scrolled through a (usually tall, with a sticky
 * child) section: 0 when its top reaches the top of the window, 1 when its
 * bottom reaches the bottom.
 */
export function useSectionProgress<T extends HTMLElement>(): [RefObject<T | null>, number] {
  const ref = useRef<T>(null)
  const [p, setP] = useState(0)
  useEffect(() => {
    let raf = 0
    const update = () => {
      raf = 0
      const el = ref.current
      if (!el) return
      const r = el.getBoundingClientRect()
      const span = r.height - window.innerHeight
      const v = span > 0 ? -r.top / span : r.top < 0 ? 1 : 0
      setP(Math.min(1, Math.max(0, v)))
    }
    const onScroll = () => { if (!raf) raf = requestAnimationFrame(update) }
    update()
    window.addEventListener('scroll', onScroll, { passive: true })
    window.addEventListener('resize', onScroll)
    return () => { window.removeEventListener('scroll', onScroll); window.removeEventListener('resize', onScroll); cancelAnimationFrame(raf) }
  }, [])
  return [ref, p]
}

/** Where a value sits between a and b, clamped to 0..1 (for timing parts of a scroll). */
export const seg = (p: number, a: number, b: number) => Math.min(1, Math.max(0, (p - a) / (b - a)))

/** A number counting up to `to` once `run` is true. */
export function useCountUp(to: number, run: boolean, ms = 1400) {
  const [v, setV] = useState(0)
  useEffect(() => {
    if (!run) return
    if (reducedMotion()) { setV(to); return }
    const t0 = performance.now()
    let raf = 0
    const step = (t: number) => {
      const k = Math.min(1, (t - t0) / ms)
      setV(to * (1 - Math.pow(1 - k, 3)))
      if (k < 1) raf = requestAnimationFrame(step)
    }
    raf = requestAnimationFrame(step)
    return () => cancelAnimationFrame(raf)
  }, [to, run, ms])
  return v
}
