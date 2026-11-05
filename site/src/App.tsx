import { BootScrub, FaceRail, Hero, Nav } from './components/Story'
import { AI, AppSection, Features, Footer, Games, GetIt, Health, Power, Tech } from './components/Sections'

export default function App() {
  return (
    <>
      <Nav />
      <main>
        <Hero />
        <BootScrub />
        <FaceRail />
        <Features />
        <Health />
        <AI />
        <Games />
        <Power />
        <AppSection />
        <Tech />
        <GetIt />
      </main>
      <Footer />
    </>
  )
}
