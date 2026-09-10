const colors = {
  brand: 'bg-purple-500/10 text-purple-700 dark:text-purple-300 border-purple-500/20 dark:border-purple-500/30',
  gray: 'bg-gray-100 text-gray-700 dark:bg-purple-950/40 dark:text-gray-300 border-gray-200 dark:border-purple-800/30',
  green: 'bg-emerald-500/10 text-emerald-700 dark:text-emerald-300 border-emerald-500/20 dark:border-emerald-500/30',
  amber: 'bg-amber-500/10 text-amber-700 dark:text-amber-300 border-amber-500/20 dark:border-amber-500/30',
  red: 'bg-rose-500/10 text-rose-700 dark:text-rose-300 border-rose-500/20 dark:border-rose-500/30',
}

const dotColors = {
  brand: 'bg-purple-500',
  gray: 'bg-gray-400 dark:bg-gray-500',
  green: 'bg-emerald-500',
  amber: 'bg-amber-500',
  red: 'bg-rose-500',
}

const sizes = {
  sm: 'px-2 py-0.5 text-[11px]',
  md: 'px-2.5 py-0.5 text-xs',
  lg: 'px-3 py-1 text-sm',
}

export default function Badge({
  children,
  color = 'brand',
  size = 'md',
  dot = false,
  className = '',
}) {
  const chosenColor = colors[color] || colors.brand
  const chosenDot = dotColors[color] || dotColors.brand
  const chosenSize = sizes[size] || sizes.md

  return (
    <span
      className={`inline-flex items-center gap-1.5 rounded-full font-medium border ${chosenColor} ${chosenSize} ${className}`}
    >
      {dot && <span className={`h-1.5 w-1.5 rounded-full ${chosenDot}`} aria-hidden="true" />}
      {children}
    </span>
  )
}
