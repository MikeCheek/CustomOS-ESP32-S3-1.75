import { useEffect, useState, type ReactNode } from 'react'
import { Loop, Phone, Shot, Watch } from './Devices'
import { seg, useCountUp, useInView, useSectionProgress } from '../lib'

const REPO = 'https://github.com/MikeCheek/CustomOS-ESP32-S3-Amoled'

function Reveal({ children, delay = 0, className = '' }: { children: ReactNode; delay?: number; className?: string }) {
  const [ref, seen] = useInView<HTMLDivElement>(0.15)
  return <div ref={ref} className={`reveal ${seen ? 'in' : ''} ${className}`} style={{ transitionDelay: `${delay}ms` }}>{children}</div>
}

function Heading({ kicker, title, sub }: { kicker: string; title: ReactNode; sub?: string }) {
  return (
    <Reveal className="mx-auto mb-14 max-w-3xl text-center">
      <p className="text-sm font-medium uppercase tracking-[0.3em] text-accent">{kicker}</p>
      <h2 className="mt-3 text-4xl font-semibold tracking-tight md:text-6xl">{title}</h2>
      {sub && <p className="mx-auto mt-5 max-w-2xl text-lg text-ink">{sub}</p>}
    </Reveal>
  )
}

/** Everyday features, a bento grid of cards with the real screens. */
export function Features() {
  const cards: { shot: string; title: string; text: string; wide?: boolean; loop?: boolean }[] = [
    { shot: 'notifications.png', title: 'Notifications', text: 'App icons, quick and voice replies, smart replies written on your phone, stacked by app, swipe to clear.', wide: true },
    { shot: 'call.png', title: 'Calls', text: 'Answer or decline from the wrist.' },
    { shot: 'anim_music', title: 'Music', text: 'The phone\'s player, album art, a 3D spectrum ring.', loop: true },
    { shot: 'navigation.png', title: 'Turn by turn', text: 'Google Maps directions come up by themselves.' },
    { shot: 'wallet_aztec.png', title: 'Wallet', text: 'Boarding passes and loyalty cards, scanned from a screenshot, full brightness.', wide: true },
    { shot: 'camera_remote.png', title: 'Camera remote', text: 'A live view, shutter and self-timer - even for the phone\'s own camera app.' },
    { shot: 'calendar.png', title: 'Calendar', text: 'The agenda and reminders that work offline.' },
    { shot: 'briefing.png', title: 'Morning briefing', text: 'Sleep, weather and the day, at the first look.' },
    { shot: 'weather.png', title: 'Weather', text: 'Hours and days, and animated skies behind the face.' },
    { shot: 'timer_running.png', title: 'Timers & alarms', text: 'Timer, stopwatch with laps, alarms, world clock.' },
    { shot: 'draw_open.png', title: 'Draw to open', text: 'One stroke - A for the assistant, M for music.' },
  ]
  return (
    <section className="mx-auto max-w-6xl px-5 py-32">
      <Heading kicker="Every day" title={<>Everything your phone knows,<br /><span className="text-ink">on your wrist.</span></>} />
      <div className="grid grid-cols-1 gap-5 sm:grid-cols-2 lg:grid-cols-4">
        {cards.map((c, i) => (
          <Reveal key={c.title} delay={(i % 4) * 90} className={c.wide ? 'sm:col-span-2' : ''}>
            <div className={`group flex h-full flex-col items-center gap-5 overflow-hidden rounded-3xl border border-white/8 bg-gradient-to-b from-white/[0.06] to-white/[0.01] p-6 transition hover:border-accent/40 ${c.wide ? 'sm:flex-row' : ''}`}>
              <div className="shrink-0 transition duration-500 group-hover:scale-105">
                <Watch size={c.wide ? 160 : 120} strap={false}>{c.loop ? <Loop name={c.shot} alt={c.title} /> : <Shot name={c.shot} alt={c.title} />}</Watch>
              </div>
              <div className={`text-center ${c.wide ? 'sm:text-left' : ''}`}>
                <h3 className="text-xl font-semibold">{c.title}</h3>
                <p className="mt-2 text-sm leading-relaxed text-ink">{c.text}</p>
              </div>
            </div>
          </Reveal>
        ))}
      </div>
    </section>
  )
}

/** Health: three watches rising at their own pace with the scroll. */
export function Health() {
  const [ref, p] = useSectionProgress<HTMLElement>()
  const small = typeof window !== 'undefined' && window.innerWidth < 640 ? 0.48 : 1   // three side by side on a phone
  const w: [string, string, number][] = [['activity_rings.png', 'Rings', 0], ['sleep_app.png', 'Sleep', 0.15], ['fall_alert.png', 'Fall detection', 0.3]]
  return (
    <section id="health" ref={ref} className="relative h-[220vh]">
      <div className="sticky top-0 flex h-screen flex-col items-center justify-center overflow-hidden px-5">
        <div className="pointer-events-none absolute inset-0" style={{ background: 'radial-gradient(50% 40% at 50% 60%, rgba(61,220,132,.15), transparent 70%)' }} />
        <div className="mb-12 text-center" style={{ opacity: seg(p, 0, 0.2) }}>
          <p className="text-sm font-medium uppercase tracking-[0.3em] text-good">Health</p>
          <h2 className="mt-3 text-4xl font-semibold tracking-tight md:text-6xl">Moves with you.<br /><span className="text-ink">Watches over you.</span></h2>
        </div>
        <div className="flex items-end justify-center gap-2 sm:gap-6 md:gap-12">
          {w.map(([n, t, d], i) => {
            const k = seg(p, 0.1 + d, 0.45 + d)
            return (
              <figure key={n} className="text-center" style={{ opacity: k, transform: `translateY(${(1 - k) * 160}px) scale(${0.85 + 0.15 * k})` }}>
                <Watch size={(i === 1 ? 220 : 180) * small} strap={false}><Shot name={n} alt={t} /></Watch>
                <figcaption className="mt-4 font-medium">{t}</figcaption>
              </figure>
            )
          })}
        </div>
        <p className="mt-12 max-w-2xl text-center text-ink" style={{ opacity: seg(p, 0.6, 0.8) }}>
          Steps from the hardware pedometer, Move / Exercise / Stand rings, 9 workout types with GPS, sleep from wrist movement with a smart alarm,
          Breathe by sound, and fall detection that texts your emergency contact. All of it written to Health Connect or Apple Health.
        </p>
      </div>
    </section>
  )
}

/** The assistant and the memos: the AI runs on the phone. A chat that types itself. */
export function AI() {
  const [ref, seen] = useInView<HTMLDivElement>(0.3)
  const q = 'What did we decide about the trip?'
  const a = 'You agreed on Sicily, the second week of June - Marco books the flights, you look for a place in Siracusa.'
  const [n, setN] = useState(0)
  useEffect(() => {
    if (!seen) return
    let i = 0
    const id = setInterval(() => { i++; setN(i); if (i > q.length + a.length + 20) clearInterval(id) }, 32)
    return () => clearInterval(id)
  }, [seen])
  const qs = q.slice(0, n), as = n > q.length + 12 ? a.slice(0, n - q.length - 12) : ''
  const models = ['Whisper', 'Moonshine', 'SenseVoice', 'Qwen3', 'Gemma 3', 'EmbeddingGemma', 'FastVLM', 'Gemma 3n', 'Piper voices', 'pyannote', 'CAM++', 'CED sound tags', 'WakeNet', 'MultiNet']
  return (
    <section id="ai" className="relative overflow-hidden py-32">
      <div className="pointer-events-none absolute inset-0" style={{ background: 'radial-gradient(45% 45% at 30% 50%, rgba(124,108,255,.22), transparent 70%)' }} />
      <div className="mx-auto grid max-w-6xl items-center gap-16 px-5 md:grid-cols-2">
        <div ref={ref} className="flex justify-center">
          <Phone className="float">
            <div className="flex h-full flex-col gap-3 p-5 pt-14 text-sm">
              <p className="text-center text-xs uppercase tracking-widest text-ink">Ask AI · your memos</p>
              {qs && <div className="self-end rounded-2xl rounded-br-sm bg-accent px-3.5 py-2.5">{qs}</div>}
              {as && <div className={`self-start rounded-2xl rounded-bl-sm bg-white/10 px-3.5 py-2.5 ${as.length < a.length ? 'caret' : ''}`}>{as}</div>}
              <div className="mt-auto rounded-2xl border border-white/10 p-3 text-xs text-ink">
                <p className="font-medium text-white">Trip planning · 4:12</p>
                <p className="mt-1">To do: book flights · find a place in Siracusa</p>
                <p className="mt-1">People: Marco, Giulia</p>
              </div>
              <p className="text-center text-[10px] text-ink">Runs on this phone · nothing goes online</p>
            </div>
          </Phone>
        </div>
        <div>
          <Reveal><p className="text-sm font-medium uppercase tracking-[0.3em] text-accent">On-phone AI</p></Reveal>
          <Reveal delay={100}><h2 className="mt-3 text-4xl font-semibold tracking-tight md:text-6xl">Private by design.</h2></Reveal>
          <Reveal delay={200}>
            <p className="mt-6 text-lg text-ink">
              Record a memo on the watch. The phone transcribes it, splits who said what, writes a title, a summary and the to-dos,
              and lets you ask about it - by meaning, so "the trip" finds "holiday in Sicily". The assistant answers out loud,
              Look describes a photo, and sound alerts tell you the doorbell rang. Every model is a download you choose.
            </p>
          </Reveal>
          <Reveal delay={300}>
            <div className="mt-8 flex flex-wrap gap-2">
              {models.map((m) => <span key={m} className="rounded-full border border-white/10 bg-white/5 px-3 py-1 text-xs text-ink">{m}</span>)}
            </div>
          </Reveal>
          <Reveal delay={400}>
            <div className="mt-10 flex gap-6">
              <Watch size={130} strap={false}><Shot name="assistant_listen.png" alt="The assistant listening" /></Watch>
              <Watch size={130} strap={false}><Shot name="assistant_answer.png" alt="The assistant's answer" /></Watch>
            </div>
          </Reveal>
        </div>
      </div>
    </section>
  )
}

/** Games: the top five, each card drifting at its own speed. */
export function Games() {
  const [ref, p] = useSectionProgress<HTMLElement>()
  const g: { shot: string; loop?: boolean; name: string; text: string }[] = [
    { shot: 'anim_piano', loop: true, name: 'Piano Tiles', text: '42 songs, a real piano synth, your own songs from a hum or a MIDI file.' },
    { shot: 'casino.png', name: 'Casino', text: 'Slots, blackjack, video poker, roulette and dice on one floor, one wallet.' },
    { shot: 'game_maze.png', name: 'Maze Raider', text: 'A 3D raid through a new maze every level.' },
    { shot: 'roulette_spin.png', name: 'Roulette', text: 'The ball runs, drops and settles on the frets.' },
    { shot: 'game_breakout.png', name: 'Breakout', text: 'An arc paddle on the rim, 50 levels, bosses.' },
  ]
  return (
    <section id="games" ref={ref} className="relative overflow-hidden py-32">
      <Heading kicker="Play" title={<>Games made<br /><span className="text-ink">for a circle.</span></>} sub="Played by touch, by tilt, with the phone as a gamepad, or an ESP-NOW joystick. Also: Flappy, Fruit Slice, Ninja Dungeon, Snake, Runner, Plane, Dodger, Simon, Reaction." />
      <div className="mx-auto flex max-w-6xl flex-wrap justify-center gap-8 px-5">
        {g.map((x, i) => (
          <div key={x.name} className="w-56 text-center" style={{ transform: `translateY(${(0.5 - p) * (i % 2 ? 140 : -80)}px)` }}>
            <Watch size={190} strap={false}>{x.loop ? <Loop name={x.shot} alt={x.name} /> : <Shot name={x.shot} alt={x.name} />}</Watch>
            <h3 className="mt-5 text-lg font-semibold">{x.name}</h3>
            <p className="mt-1 text-sm text-ink">{x.text}</p>
          </div>
        ))}
      </div>
    </section>
  )
}

function Stat({ to, unit, label, run, dec = 0 }: { to: number; unit: string; label: string; run: boolean; dec?: number }) {
  const v = useCountUp(to, run)
  return (
    <div className="text-center">
      <p className="text-5xl font-semibold tracking-tight md:text-6xl">{v.toFixed(dec)}<span className="text-2xl text-ink">{unit}</span></p>
      <p className="mt-2 text-sm text-ink">{label}</p>
    </div>
  )
}

/** Power: a battery ring that fills as you scroll, and the numbers behind it. */
export function Power() {
  const [ref, p] = useSectionProgress<HTMLElement>()
  const [sref, seen] = useInView<HTMLDivElement>(0.3)
  const k = seg(p, 0.1, 0.6)
  const C = 2 * Math.PI * 130
  return (
    <section id="power" ref={ref} className="relative h-[200vh]">
      <div className="sticky top-0 flex h-screen flex-col items-center justify-center overflow-hidden px-5">
        <div className="pointer-events-none absolute inset-0" style={{ background: 'radial-gradient(40% 40% at 50% 45%, rgba(255,194,74,.12), transparent 70%)' }} />
        <p className="text-sm font-medium uppercase tracking-[0.3em] text-warn">Power</p>
        <h2 className="mt-3 text-center text-4xl font-semibold tracking-tight md:text-6xl">Sleeps whenever it can.</h2>
        <div className="relative mt-10">
          <svg width="300" height="300" viewBox="0 0 300 300" className="-rotate-90">
            <circle cx="150" cy="150" r="130" fill="none" stroke="rgba(255,255,255,.08)" strokeWidth="18" />
            <circle cx="150" cy="150" r="130" fill="none" stroke="url(#bat)" strokeWidth="18" strokeLinecap="round" strokeDasharray={C} strokeDashoffset={C * (1 - k * 0.8)} />
            <defs><linearGradient id="bat" x1="0" x2="1"><stop offset="0" stopColor="#3ddc84" /><stop offset="1" stopColor="#ffc24a" /></linearGradient></defs>
          </svg>
          <div className="absolute inset-0 flex flex-col items-center justify-center">
            <Watch size={170} strap={false}><Shot name="battery.png" alt="The Battery screen" /></Watch>
          </div>
        </div>
        <div ref={sref} className="mt-12 grid w-full max-w-4xl grid-cols-2 gap-8 md:grid-cols-4" style={{ opacity: seg(p, 0.35, 0.55) }}>
          <Stat to={1000} unit=" mAh" label="the Li-Po it is tuned for" run={seen} />
          <Stat to={550} unit=" ms" label="Bluetooth interval when idle" run={seen} />
          <Stat to={80} unit=" %" label="Optimized charging holds at" run={seen} />
          <Stat to={30} unit=" s" label="screen timeout, by default" run={seen} />
        </div>
        <p className="mt-10 max-w-2xl text-center text-sm text-ink" style={{ opacity: seg(p, 0.5, 0.7) }}>
          Light sleep with the phone still connected, wrist-down to sleep and raise-to-wake only when you look, a slower CPU between redraws,
          four battery modes, and Diagnostics that names whatever kept the watch awake - with the fix.
        </p>
      </div>
    </section>
  )
}

/** The companion app, and the watch floating over it. */
export function AppSection() {
  const [ref, p] = useSectionProgress<HTMLElement>()
  const k = seg(p, 0.15, 0.55)
  const rows = ['Faces & photo faces', 'Home gestures & Tiles', 'Voice memos & Ask AI', 'Wallet', 'Camera remote', 'Diagnostics', 'Updates for both']
  return (
    <section id="app" ref={ref} className="relative h-[220vh]">
      <div className="sticky top-0 flex h-screen items-center overflow-hidden px-5">
        <div className="mx-auto grid w-full max-w-6xl items-center gap-12 md:grid-cols-2">
          <div>
            <p className="text-sm font-medium uppercase tracking-[0.3em] text-accent2">The companion app</p>
            <h2 className="mt-3 text-4xl font-semibold tracking-tight md:text-6xl">A watch in your phone, too.</h2>
            <p className="mt-6 text-lg text-ink">
              Android and iPhone. It runs the watch's own firmware as an emulator - with no watch, it is the watch, and on Android it floats
              over every app as a round bubble you can pinch, tuck away at the edge, or drop on the ✕.
            </p>
            <ul className="mt-8 space-y-2">
              {rows.map((r, i) => (
                <li key={r} className="flex items-center gap-3" style={{ opacity: seg(p, 0.15 + i * 0.05, 0.3 + i * 0.05), transform: `translateX(${(1 - seg(p, 0.15 + i * 0.05, 0.3 + i * 0.05)) * -30}px)` }}>
                  <span className="h-1.5 w-1.5 rounded-full bg-accent2" />{r}
                </li>
              ))}
            </ul>
          </div>
          <div className="relative hidden justify-center md:flex">
            <div style={{ transform: `translateY(${(1 - k) * 120}px) rotate(${(1 - k) * 8}deg)`, opacity: 0.3 + 0.7 * k }}>
              <Phone>
                <div className="flex h-full flex-col p-5 pt-14">
                  <p className="text-xs uppercase tracking-widest text-ink">Watch emulator</p>
                  <div className="mt-6 flex justify-center"><Watch size={170} strap={false}><Shot name="watchface.png" alt="The emulated watch" /></Watch></div>
                  <div className="mt-6 grid grid-cols-2 gap-2 text-xs">
                    {['BOOT', 'PWR', 'Float', 'Keep as my watch'].map((b) => <div key={b} className="rounded-xl bg-white/8 py-2.5 text-center">{b}</div>)}
                  </div>
                </div>
              </Phone>
            </div>
            {/* the floating watch, drifting off the phone */}
            <div className="absolute right-0 top-6 md:right-4" style={{ transform: `translate(${k * 10}px, ${-k * 24}px) scale(${0.6 + 0.4 * k})`, opacity: k }}>
              <div className="float"><Watch size={110} strap={false}><Loop name="anim_music" alt="The floating watch" /></Watch></div>
            </div>
          </div>
        </div>
      </div>
    </section>
  )
}

export function Tech() {
  const items: [string, string][] = [
    ['ESP32-S3', 'Dual core 240 MHz, 8 MB PSRAM, 16 MB flash, 6 MB app slots'],
    ['466 × 466 AMOLED', 'CO5300, a full framebuffer, ~3x faster sends'],
    ['Bluetooth LE + Wi-Fi', 'Encrypted pairing, 2M PHY, fast Wi-Fi transfers'],
    ['Signed updates', 'ECDSA P-256 over Bluetooth or Wi-Fi, automatic rollback'],
    ['Sensors', 'QMI8658 IMU, AXP2101 PMU, two mics, speaker, SD card'],
    ['Tested every push', 'Two firmware builds, PC tests, emulator self-tests, the app'],
  ]
  return (
    <section className="mx-auto max-w-6xl px-5 py-32">
      <Heading kicker="Under the hood" title="Open, top to bottom." sub="Arduino + Arduino_GFX, no LVGL. The simulator renders every screenshot on this page from the real UI code." />
      <div className="grid gap-5 sm:grid-cols-2 lg:grid-cols-3">
        {items.map(([h, t], i) => (
          <Reveal key={h} delay={(i % 3) * 100}>
            <div className="h-full rounded-3xl border border-white/8 bg-white/[0.03] p-7">
              <h3 className="text-xl font-semibold">{h}</h3>
              <p className="mt-2 text-sm text-ink">{t}</p>
            </div>
          </Reveal>
        ))}
      </div>
    </section>
  )
}

export function GetIt() {
  const steps: [string, string, string][] = [
    ['1', 'Flash the watch', 'Download the firmware .bin from the latest release and flash it once over USB - updates come over the air after that.'],
    ['2', 'Install the app', 'The Android APK is in the same release; on iPhone, the unsigned .ipa with Sideloadly or AltStore, or build it in Xcode.'],
    ['3', 'Pair', 'The watch shows a code, the app asks for it - an encrypted link from then on.'],
  ]
  return (
    <section className="relative overflow-hidden py-32">
      <div className="pointer-events-none absolute inset-0" style={{ background: 'radial-gradient(50% 50% at 50% 100%, rgba(124,108,255,.25), transparent 70%)' }} />
      <Heading kicker="Get it" title={<>Free. Open source.<br /><span className="text-ink">Yours to change.</span></>} />
      <div className="mx-auto grid max-w-5xl gap-6 px-5 md:grid-cols-3">
        {steps.map(([n, h, t], i) => (
          <Reveal key={n} delay={i * 120}>
            <div className="h-full rounded-3xl border border-white/8 bg-white/[0.03] p-7">
              <span className="flex h-9 w-9 items-center justify-center rounded-full bg-accent font-semibold">{n}</span>
              <h3 className="mt-5 text-xl font-semibold">{h}</h3>
              <p className="mt-2 text-sm text-ink">{t}</p>
            </div>
          </Reveal>
        ))}
      </div>
      <Reveal className="mt-14 flex justify-center gap-3">
        <a href={`${REPO}/releases/latest`} className="rounded-full bg-accent px-7 py-3 font-medium transition hover:brightness-110">Latest release</a>
        <a href={REPO} className="rounded-full border border-white/20 px-7 py-3 font-medium transition hover:bg-white/10">Source on GitHub</a>
      </Reveal>
    </section>
  )
}

export function Footer() {
  return (
    <footer className="border-t border-white/5 py-10 text-center text-xs text-ink">
      <p>AmoledWatch OS for the Waveshare ESP32-S3-Touch-AMOLED-1.75 · every watch screen here is rendered from the firmware's own code.</p>
      <p className="mt-2"><a className="underline hover:text-white" href={REPO}>github.com/MikeCheek/CustomOS-ESP32-S3-Amoled</a></p>
    </footer>
  )
}
