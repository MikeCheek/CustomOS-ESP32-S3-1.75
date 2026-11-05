import type { ReactNode } from 'react'
import { asset } from '../lib'

/**
 * The watch, drawn in CSS: a round brushed-metal case with BOOT and PWR on the
 * right, a strap above and below, and the 466 x 466 screen inside (a screenshot,
 * a GIF / video or a canvas, passed as children).
 */
export function Watch({ children, size = 320, strap = true, className = '' }: { children: ReactNode; size?: number; strap?: boolean; className?: string }) {
  const s = size
  return (
    <div className={`relative ${className}`} style={{ width: s * 1.12, height: strap ? s * 1.9 : s * 1.12 }}>
      {strap && (
        <>
          <div className="absolute left-1/2 -translate-x-1/2 rounded-t-[40%]" style={{ top: 0, width: s * 0.62, height: s * 0.6, background: 'linear-gradient(180deg,#0b0b0e,#1d1d24 70%,#26262e)' }} />
          <div className="absolute left-1/2 -translate-x-1/2 rounded-b-[40%]" style={{ bottom: 0, width: s * 0.62, height: s * 0.6, background: 'linear-gradient(0deg,#0b0b0e,#1d1d24 70%,#26262e)' }} />
        </>
      )}
      {/* the buttons */}
      <div className="absolute rounded-r-md" style={{ right: -s * 0.025, top: '43%', width: s * 0.05, height: s * 0.09, background: 'linear-gradient(90deg,#55555c,#9a9aa2)' }} />
      <div className="absolute rounded-r-md" style={{ right: -s * 0.025, top: '53%', width: s * 0.05, height: s * 0.07, background: 'linear-gradient(90deg,#55555c,#9a9aa2)' }} />
      {/* the case */}
      <div
        className="absolute left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2 rounded-full"
        style={{ width: s * 1.12, height: s * 1.12, background: 'conic-gradient(from 210deg,#3b3b42,#8d8d95,#2c2c31,#6a6a72,#3b3b42)', boxShadow: '0 40px 80px -20px rgba(0,0,0,.9), inset 0 0 0 2px rgba(255,255,255,.08)' }}
      >
        <div className="absolute inset-[3.5%] rounded-full bg-black" style={{ boxShadow: 'inset 0 0 18px rgba(0,0,0,.9)' }}>
          <div className="absolute inset-[3%] overflow-hidden rounded-full bg-black">{children}</div>
          {/* glass */}
          <div className="pointer-events-none absolute inset-[3%] rounded-full" style={{ background: 'linear-gradient(135deg,rgba(255,255,255,.12),transparent 40%)' }} />
        </div>
      </div>
    </div>
  )
}

/** A screenshot from the simulator, filling the watch's round screen. */
export function Shot({ name, alt }: { name: string; alt: string }) {
  return <img src={asset(`shots/${name}`)} alt={alt} loading="lazy" className="h-full w-full object-cover" draggable={false} />
}

/** An animation: the MP4 the build made from the GIF (or the GIF if there's none). */
export function Loop({ name, alt }: { name: string; alt: string }) {
  return (
    <video className="h-full w-full object-cover" autoPlay muted loop playsInline poster={asset(`shots/${name}.gif`)} aria-label={alt}>
      <source src={asset(`media/${name}.mp4`)} type="video/mp4" />
      <img src={asset(`shots/${name}.gif`)} alt={alt} />
    </video>
  )
}

/** A phone, drawn in CSS, with whatever screen is given. */
export function Phone({ children, className = '' }: { children: ReactNode; className?: string }) {
  return (
    <div className={`relative h-[560px] w-[272px] rounded-[44px] p-[10px] ${className}`} style={{ background: 'linear-gradient(145deg,#3a3a40,#16161a)', boxShadow: '0 50px 100px -30px rgba(0,0,0,.9), inset 0 0 0 1.5px rgba(255,255,255,.1)' }}>
      <div className="relative h-full w-full overflow-hidden rounded-[36px] bg-[#0b0b10]">
        <div className="absolute left-1/2 top-2 z-10 h-6 w-24 -translate-x-1/2 rounded-full bg-black" />
        {children}
      </div>
    </div>
  )
}
