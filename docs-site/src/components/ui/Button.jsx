import { Link } from 'react-router-dom'

const base =
  'inline-flex items-center justify-center gap-2 font-medium rounded-lg transition-all duration-150 focus:outline-none focus-visible:ring-2 focus-visible:ring-brand-500 focus-visible:ring-offset-2 dark:focus-visible:ring-offset-[#0a0714] disabled:opacity-50 disabled:pointer-events-none active:scale-[0.98]'

const variants = {
  primary:
    'bg-gradient-to-r from-brand-600 to-brand-700 text-white shadow-sm shadow-brand-500/25 hover:from-brand-500 hover:to-brand-600 hover:shadow-md hover:shadow-brand-500/30 border border-brand-500/30',
  secondary:
    'bg-white text-gray-800 border border-gray-200 shadow-sm hover:bg-gray-50 hover:border-gray-300 dark:bg-[#181230] dark:text-gray-200 dark:border-purple-500/30 dark:hover:bg-[#221a42] dark:hover:border-purple-500/50 hover:shadow-sm',
  outline:
    'border border-brand-500/40 text-brand-700 hover:bg-brand-50/80 dark:border-brand-400/40 dark:text-brand-300 dark:hover:bg-brand-950/50',
  ghost:
    'text-gray-600 hover:bg-gray-100 hover:text-gray-900 dark:text-gray-300 dark:hover:bg-purple-950/40 dark:hover:text-purple-200',
}

const sizes = {
  sm: 'text-xs px-3 py-1.5 rounded-md font-medium',
  md: 'text-sm px-4 py-2 rounded-lg font-medium',
  lg: 'text-base px-5 py-2.5 rounded-lg font-medium',
}

export default function Button({
  children,
  variant = 'primary',
  size = 'md',
  href,
  to,
  external = false,
  className = '',
  ...rest
}) {
  const cls = `${base} ${variants[variant] || variants.primary} ${sizes[size] || sizes.md} ${className}`

  if (to) {
    return (
      <Link to={to} className={cls} {...rest}>
        {children}
      </Link>
    )
  }

  if (href) {
    return (
      <a
        href={href}
        className={cls}
        {...(external ? { target: '_blank', rel: 'noopener noreferrer' } : {})}
        {...rest}
      >
        {children}
      </a>
    )
  }

  return (
    <button type="button" className={cls} {...rest}>
      {children}
    </button>
  )
}
