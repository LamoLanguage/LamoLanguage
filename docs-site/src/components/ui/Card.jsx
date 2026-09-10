export default function Card({
  icon,
  title,
  children,
  className = '',
}) {
  return (
    <div
      className={`group relative overflow-hidden rounded-xl border border-gray-200/80 bg-white p-6 shadow-sm transition-all duration-200 hover:border-purple-400/50 hover:shadow-md dark:border-purple-500/20 dark:bg-[#120d24]/80 dark:backdrop-blur-sm dark:hover:border-purple-500/40 dark:hover:shadow-glow-sm ${className}`}
    >
      {/* Subtle purple hover gradient overlay */}
      <div className="pointer-events-none absolute inset-0 bg-gradient-to-br from-purple-500/5 via-transparent to-transparent opacity-0 transition-opacity duration-300 group-hover:opacity-100 dark:from-purple-500/10" />

      <div className="relative z-10">
        {icon && (
          <div className="mb-4 inline-flex h-10 w-10 items-center justify-center rounded-lg bg-purple-50 text-purple-600 ring-1 ring-purple-500/20 dark:bg-purple-950/60 dark:text-purple-300 dark:ring-purple-500/30 transition-transform duration-200 group-hover:scale-105">
            {icon}
          </div>
        )}
        {title && (
          <h3 className="mb-2 text-base font-semibold text-gray-900 dark:text-white">
            {title}
          </h3>
        )}
        <div className="text-sm leading-6 text-gray-600 dark:text-gray-300">
          {children}
        </div>
      </div>
    </div>
  )
}
