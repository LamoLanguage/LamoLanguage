const styles = {
  info: {
    wrap: 'bg-purple-50/80 text-purple-950 border border-purple-500/20 dark:bg-purple-950/30 dark:text-purple-200 dark:border-purple-500/30',
    icon: (
      <svg className="h-5 w-5 shrink-0 text-purple-600 dark:text-purple-400 mt-0.5" viewBox="0 0 20 20" fill="currentColor" aria-hidden="true">
        <path
          fillRule="evenodd"
          d="M18 10a8 8 0 11-16 0 8 8 0 0116 0zm-7-4a1 1 0 11-2 0 1 1 0 012 0zM9 9a.75.75 0 000 1.5h.253a.25.25 0 01.244.304l-.459 2.066A1.75 1.75 0 0010.747 15H11a.75.75 0 000-1.5h-.253a.25.25 0 01-.244-.304l.459-2.066A1.75 1.75 0 009.253 9H9z"
          clipRule="evenodd"
        />
      </svg>
    ),
  },
  warning: {
    wrap: 'bg-amber-50/80 text-amber-950 border border-amber-500/20 dark:bg-amber-950/30 dark:text-amber-200 dark:border-amber-500/30',
    icon: (
      <svg className="h-5 w-5 shrink-0 text-amber-600 dark:text-amber-400 mt-0.5" viewBox="0 0 20 20" fill="currentColor" aria-hidden="true">
        <path
          fillRule="evenodd"
          d="M8.485 2.495c.673-1.167 2.357-1.167 3.03 0l6.28 10.875c.673 1.167-.17 2.625-1.516 2.625H3.72c-1.347 0-2.189-1.458-1.515-2.625L8.485 2.495zM10 6a.75.75 0 01.75.75v3.5a.75.75 0 01-1.5 0v-3.5A.75.75 0 0110 6zm0 9a1 1 0 100-2 1 1 0 000 2z"
          clipRule="evenodd"
        />
      </svg>
    ),
  },
}

export default function Alert({ type = 'info', title, children }) {
  const s = styles[type] ?? styles.info
  return (
    <div className={`flex gap-3 rounded-xl p-4 shadow-sm ${s.wrap}`}>
      {s.icon}
      <div className="text-sm leading-6">
        {title && <p className="font-semibold mb-0.5">{title}</p>}
        <div className="opacity-95">{children}</div>
      </div>
    </div>
  )
}
