import { useState } from 'react'
import { NavLink, useLocation } from 'react-router-dom'

export const docsSections = [
  {
    title: 'Start',
    items: [
      { label: 'Getting Started', to: '/getting-started' },
      { label: 'CLI Reference', to: '/cli' },
    ],
  },
  {
    title: 'Language Guide',
    items: [
      { label: 'Overview', to: '/docs' },
      { label: 'Variables & Types', to: '/docs#variables' },
      { label: 'Control Flow', to: '/docs#control-flow' },
      { label: 'Functions', to: '/docs#functions' },
      { label: 'Structs & Methods', to: '/docs#structs' },
      { label: 'Enums & Match', to: '/docs#enums-match' },
      { label: 'Generics', to: '/docs#generics' },
      { label: 'Traits', to: '/docs#traits', badge: 'v2.10' },
      { label: 'Memory & Style', to: '/docs#memory-style' },
    ],
  },
  {
    title: 'Standard Library',
    items: [{ label: '15 Modules', to: '/stdlib' }],
  },
]

function Section({ title, items, startOpen }) {
  const [open, setOpen] = useState(startOpen)
  const location = useLocation()

  return (
    <div className="mb-4">
      <button
        type="button"
        onClick={() => setOpen((v) => !v)}
        className="flex w-full items-center justify-between rounded-lg px-3 py-1.5 text-xs font-semibold uppercase tracking-wider text-gray-500 hover:text-gray-900 dark:text-purple-300/60 dark:hover:text-purple-200 transition-colors"
      >
        <span>{title}</span>
        <svg
          className={`h-3.5 w-3.5 transition-transform duration-200 ${open ? 'rotate-90 text-purple-600 dark:text-purple-400' : 'text-gray-400'}`}
          viewBox="0 0 20 20"
          fill="currentColor"
          aria-hidden="true"
        >
          <path
            fillRule="evenodd"
            d="M7.21 14.77a.75.75 0 01.02-1.06L11.168 10 7.23 6.29a.75.75 0 111.04-1.08l4.5 4.25a.75.75 0 010 1.08l-4.5 4.25a.75.75 0 01-1.06-.02z"
            clipRule="evenodd"
          />
        </svg>
      </button>
      {open && (
        <ul className="mt-1 space-y-0.5 border-l border-gray-200 dark:border-purple-900/30 pl-2">
          {items.map((item) => {
            const hasHash = item.to.includes('#')
            const isExactHash = hasHash && location.pathname + location.hash === item.to
            const isExactPath = !hasHash && location.pathname === item.to

            const isActive = isExactHash || isExactPath

            return (
              <li key={item.to}>
                <NavLink
                  to={item.to}
                  onClick={(e) => {
                    if (hasHash) {
                      const hash = item.to.split('#')[1]
                      const el = document.getElementById(hash)
                      if (el) {
                        el.scrollIntoView({ behavior: 'smooth', block: 'start' })
                      }
                    }
                  }}
                  className={`group flex items-center justify-between rounded-r-md px-2.5 py-1.5 text-sm transition-all duration-150 ${
                    isActive
                      ? 'border-l-2 border-purple-600 font-medium text-purple-700 bg-purple-50 dark:border-purple-400 dark:text-purple-300 dark:bg-purple-950/40 -ml-2.5 pl-3'
                      : 'text-gray-600 hover:bg-gray-100/70 hover:text-gray-900 dark:text-gray-400 dark:hover:bg-purple-950/20 dark:hover:text-gray-200'
                  }`}
                >
                  <span className="truncate">{item.label}</span>
                  {item.badge && (
                    <span className="rounded-full bg-purple-100 px-1.5 py-0.2 text-[10px] font-semibold text-purple-700 dark:bg-purple-900/60 dark:text-purple-300 ring-1 ring-inset ring-purple-600/20">
                      {item.badge}
                    </span>
                  )}
                </NavLink>
              </li>
            )
          })}
        </ul>
      )}
    </div>
  )
}

export default function Sidebar() {
  return (
    <nav aria-label="Documentation" className="space-y-1 py-1">
      {docsSections.map((s, i) => (
        <Section key={s.title} title={s.title} items={s.items} startOpen={true} />
      ))}
    </nav>
  )
}
