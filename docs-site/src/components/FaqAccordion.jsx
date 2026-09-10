import { useState } from 'react'

function Item({ question, answer, open, onToggle }) {
  return (
    <div className="border-b border-gray-200 dark:border-purple-900/30 transition-colors">
      <button
        type="button"
        onClick={onToggle}
        className="flex w-full items-center justify-between gap-4 py-5 text-left group"
      >
        <span className="text-base font-semibold text-gray-900 dark:text-white group-hover:text-purple-600 dark:group-hover:text-purple-300 transition-colors">
          {question}
        </span>
        <span
          className={`flex h-8 w-8 shrink-0 items-center justify-center rounded-full ring-1 ring-inset transition-all ${
            open
              ? 'bg-purple-50 text-purple-600 ring-purple-500/30 dark:bg-purple-950/60 dark:text-purple-300 dark:ring-purple-500/40'
              : 'text-gray-400 ring-gray-200 dark:text-gray-500 dark:ring-purple-900/40 group-hover:ring-purple-500/30'
          }`}
        >
          <svg
            className={`h-4 w-4 transition-transform duration-200 ${open ? 'rotate-180' : ''}`}
            viewBox="0 0 20 20"
            fill="currentColor"
            aria-hidden="true"
          >
            <path
              fillRule="evenodd"
              d="M5.23 7.21a.75.75 0 011.06.02L10 11.168l3.71-3.938a.75.75 0 111.08 1.04l-4.25 4.5a.75.75 0 01-1.08 0l-4.25-4.5a.75.75 0 01.02-1.06z"
              clipRule="evenodd"
            />
          </svg>
        </span>
      </button>
      {open && (
        <p className="pb-5 pr-12 text-sm leading-6 text-gray-600 dark:text-gray-300 animate-fadeIn">
          {answer}
        </p>
      )}
    </div>
  )
}

export default function FaqAccordion({ items }) {
  const [openIndex, setOpenIndex] = useState(0)
  return (
    <div className="border-t border-gray-200 dark:border-purple-900/30">
      {items.map((item, i) => (
        <Item
          key={item.question}
          question={item.question}
          answer={item.answer}
          open={openIndex === i}
          onToggle={() => setOpenIndex(openIndex === i ? -1 : i)}
        />
      ))}
    </div>
  )
}
