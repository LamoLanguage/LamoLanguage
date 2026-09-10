import { useEffect } from 'react'
import { Routes, Route, useLocation } from 'react-router-dom'
import Navbar from './components/Navbar.jsx'
import Footer from './components/Footer.jsx'
import Home from './pages/Home.jsx'
import GettingStarted from './pages/GettingStarted.jsx'
import LanguageGuide from './pages/LanguageGuide.jsx'
import StdLib from './pages/StdLib.jsx'
import Cli from './pages/Cli.jsx'

function ScrollManager() {
  const { pathname, hash } = useLocation()

  useEffect(() => {
    if (hash) {
      // Instant jump: router-driven nav shouldn't depend on smooth-scroll
      // animation, which is unavailable in some automated environments.
      const el = document.getElementById(hash.slice(1))
      if (el) {
        el.scrollIntoView({ behavior: 'instant', block: 'start' })
        return
      }
    }
    window.scrollTo(0, 0)
  }, [pathname, hash])

  return null
}

export default function App() {
  return (
    <div className="flex min-h-screen flex-col bg-white text-gray-900 dark:bg-[#0a0714] dark:text-gray-100 transition-colors duration-150">
      <ScrollManager />
      <Navbar />
      <main className="flex-1">
        <Routes>
          <Route path="/" element={<Home />} />
          <Route path="/getting-started" element={<GettingStarted />} />
          <Route path="/docs" element={<LanguageGuide />} />
          <Route path="/stdlib" element={<StdLib />} />
          <Route path="/cli" element={<Cli />} />
        </Routes>
      </main>
      <Footer />
    </div>
  )
}
