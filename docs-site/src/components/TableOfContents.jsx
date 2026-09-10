import { useEffect, useState } from 'react'

const defaultDocsHeadings = [
  { id: 'variables', title: 'Variables & Types' },
  { id: 'control-flow', title: 'Control Flow' },
  { id: 'functions', title: 'Functions' },
  { id: 'structs', title: 'Structs & Methods' },
  { id: 'enums-match', title: 'Enums & Match' },
  { id: 'generics', title: 'Generics' },
  { id: 'traits', title: 'Traits' },
  { id: 'memory-style', title: 'Memory & Style' },
]

export default function TableOfContents({ items = defaultDocsHeadings }) {
  const [activeId, setActiveId] = useState('')

  useEffect(() => {
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (entry.isIntersecting) {
            setActiveId(entry.target.id)
          }
        })
      },
      {
        rootMargin: '-80px 0% -65% 0%',
        threshold: 0,
      }
    )

    items.forEach((item) => {
      const el = document.getElementById(item.id)
      if (el) {
        observer.observe(el)
      }
    })

    return () => observer.disconnect()
  }, [items])

  const scrollTo = (e, id) => {
    e.preventDefault()
    const el = document.getElementById(id)
    if (el) {
      el.scrollIntoView({ behavior: 'smooth', block: 'start' })
      window.history.pushState(null, '', `#${id}`)
      setActiveId(id)
    }
  }

  return (
    <nav className="space-y-2 text-sm" aria-label="Table of contents">
      <div className="flex items-center gap-2 pb-2 text-xs font-semibold uppercase tracking-wider text-gray-900 dark:text-purple-200">
        <svg className="h-3.5 w-3.5 text-purple-600 dark:text-purple-400" viewBox="0 0 20 20" fill="currentColor">
          <path fillRule="evenodd" d="M3 5a1 1 0 011-1h12a1 1 0 110 2H4a1 1 0 01-1-1zm0 5a1 1 0 011-1h12a1 1 0 110 2H4a1 1 0 01-1-1zm0 5a1 1 0 011-1h6a1 1 0 110 2H4a1 1 0 01-1-1z" clipRule="evenodd" />
        </svg>
        <span>On this page</span>
      </div>
      <ul className="space-y-1 border-l border-gray-200 dark:border-purple-900/30">
        {items.map((item) => {
          const isActive = activeId === item.id
          return (
            <li key={item.id}>
              <a
                href={`#${item.id}`}
                onClick={(e) => scrollTo(e, item.id)}
                className={`-ml-px block border-l-2 py-1.5 pl-4 text-xs transition-all duration-150 ${
                  isActive
                    ? 'border-purple-600 dark:border-purple-400 font-medium text-purple-600 dark:text-purple-300 bg-purple-50/50 dark:bg-purple-950/30 rounded-r-md'
                    : 'border-transparent text-gray-500 hover:border-gray-300 hover:text-gray-900 dark:text-gray-400 dark:hover:border-purple-700/50 dark:hover:text-gray-200'
                }`}
              >
                {item.title}
              </a>
            </li>
          )
        })}
      </ul>
    </nav>
  )
}
