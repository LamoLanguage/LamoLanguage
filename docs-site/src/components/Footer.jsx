import { Link } from 'react-router-dom'

export default function Footer() {
  return (
    <footer className="border-t border-gray-200/80 bg-gray-50/70 dark:border-purple-900/30 dark:bg-[#07050e] transition-colors">
      <div className="mx-auto flex max-w-7xl flex-col items-center justify-between gap-4 px-4 py-8 sm:flex-row sm:px-6 lg:px-8">
        <div className="flex items-center gap-3">
          <img
            src="/icon.jpg"
            alt="Lamo logo"
            className="h-6 w-6 rounded-md ring-1 ring-purple-500/20"
          />
          <p className="text-sm text-gray-500 dark:text-gray-400">
            Lamo — a modern language that transpiles to C.
          </p>
          <span className="hidden sm:inline-block rounded-full bg-purple-500/10 px-2 py-0.5 text-[10px] font-medium text-purple-600 dark:text-purple-400 border border-purple-500/20">
            v2.10.0
          </span>
        </div>
        <nav className="flex items-center gap-6 text-sm">
          <Link
            to="/getting-started"
            className="text-gray-500 hover:text-purple-600 dark:text-gray-400 dark:hover:text-purple-300 transition-colors"
          >
            Get Started
          </Link>
          <Link
            to="/docs"
            className="text-gray-500 hover:text-purple-600 dark:text-gray-400 dark:hover:text-purple-300 transition-colors"
          >
            Docs
          </Link>
          <Link
            to="/stdlib"
            className="text-gray-500 hover:text-purple-600 dark:text-gray-400 dark:hover:text-purple-300 transition-colors"
          >
            Stdlib
          </Link>
          <Link
            to="/cli"
            className="text-gray-500 hover:text-purple-600 dark:text-gray-400 dark:hover:text-purple-300 transition-colors"
          >
            CLI
          </Link>
          <a
            href="https://github.com/lamo-lang/lamo"
            target="_blank"
            rel="noopener noreferrer"
            className="text-gray-500 hover:text-purple-600 dark:text-gray-400 dark:hover:text-purple-300 transition-colors"
          >
            GitHub
          </a>
        </nav>
      </div>
    </footer>
  )
}
