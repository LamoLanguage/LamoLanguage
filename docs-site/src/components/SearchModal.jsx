import { useState, useEffect, useRef } from 'react'
import { useNavigate } from 'react-router-dom'

const searchIndex = [
  // Getting Started
  {
    title: 'Prerequisites & Toolchain',
    category: 'Guide',
    url: '/getting-started',
    desc: 'C toolchain requirements (GCC, Make, Python 3) and Windows setup instructions.',
    keywords: 'gcc clang make python windows mingw build install',
  },
  {
    title: 'Build from Source',
    category: 'Guide',
    url: '/getting-started',
    desc: 'Clone the repository and build the lamo compiler with make.',
    keywords: 'git clone make test binary',
  },
  {
    title: 'Your First Lamo Program',
    category: 'Guide',
    url: '/getting-started',
    desc: 'Create hello.lamo and execute it via lamo run.',
    keywords: 'hello world run main fn print',
  },
  {
    title: 'Everyday Commands',
    category: 'Guide',
    url: '/getting-started',
    desc: 'Overview of lamo check, build, fmt, test, and repl.',
    keywords: 'check build fmt test repl command',
  },
  {
    title: 'lampm Package Manager',
    category: 'Guide',
    url: '/getting-started',
    desc: 'Scaffold new projects with lamo init and manage dependencies.',
    keywords: 'lampm init install package pkg lock dependency',
  },

  // Language Guide
  {
    title: 'Variables & Types',
    category: 'Docs',
    url: '/docs#variables',
    desc: 'let declarations, hybrid type inference, scalar types: int, float, string, bool, void.',
    keywords: 'let int float string bool void inference types array annotations',
  },
  {
    title: 'Control Flow',
    category: 'Docs',
    url: '/docs#control-flow',
    desc: 'if/else, while, for loops, break, continue, and Python-like truthiness.',
    keywords: 'if else while for break continue truthy falsy loop',
  },
  {
    title: 'Functions',
    category: 'Docs',
    url: '/docs#functions',
    desc: 'fn declaration, hoisting, parameter & return types, main function and builtins.',
    keywords: 'fn function return hoisting main print input exit len push pop',
  },
  {
    title: 'Structs & Methods',
    category: 'Docs',
    url: '/docs#structs',
    desc: 'struct definitions, typed fields, impl blocks, methods and implicit self.',
    keywords: 'struct impl method self fields object',
  },
  {
    title: 'Enums & Pattern Matching',
    category: 'Docs',
    url: '/docs#enums-match',
    desc: 'C-style enums, tagged unions with payloads, match expressions, when guards, nested patterns.',
    keywords: 'enum match pattern tagged union payload when guard option some none',
  },
  {
    title: 'Generics',
    category: 'Docs',
    url: '/docs#generics',
    desc: 'Generic structs, functions, impl blocks, and constraints (Any, Eq, Ord, Num, Hash, Show).',
    keywords: 'generics type parameter constraint ord eq num show hash any',
  },
  {
    title: 'Traits & Dictionaries',
    category: 'Docs',
    url: '/docs#traits',
    desc: 'trait contracts, impl Trait for Type, and dictionary dispatch without vtables.',
    keywords: 'trait impl contract dictionary dispatch polymorphism interface',
  },
  {
    title: 'Memory Model & Style',
    category: 'Docs',
    url: '/docs#memory-style',
    desc: 'Mark-sweep garbage collection, string arena, and AST-based lamo fmt rules.',
    keywords: 'gc garbage collection memory arena formatter fmt style',
  },

  // StdLib
  {
    title: 'std.io',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Console input/output: println, eprint, read_line, write.',
    keywords: 'io println print eprint read write console terminal',
  },
  {
    title: 'std.fs',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'File system operations: readText, writeText, exists, remove.',
    keywords: 'fs file read text write exists remove delete filesystem',
  },
  {
    title: 'std.path',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Path manipulation: join, parent, filename, normalize.',
    keywords: 'path join parent filename normalize directory',
  },
  {
    title: 'std.string',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'String utilities: length, split, trim, replace, upper, lower.',
    keywords: 'string length split trim replace upper lower text',
  },
  {
    title: 'std.math',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Math functions and constants: sqrt, pow, sin, cos, PI, floor, ceil.',
    keywords: 'math sqrt pow sin cos pi floor ceil number float',
  },
  {
    title: 'std.random',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Pseudo-random generation: int, float, bool, choice, shuffle.',
    keywords: 'random int float bool choice shuffle dice seed',
  },
  {
    title: 'std.time',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Time functions: now, sleep, monotonic timestamp.',
    keywords: 'time now sleep monotonic clock timestamp delay',
  },
  {
    title: 'std.collections',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Data structures: List, Stack, Queue, HashMap, HashSet.',
    keywords: 'collections list stack queue hashmap hashset map set dict generic',
  },
  {
    title: 'std.process',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Process operations: run, exec, currentPid.',
    keywords: 'process run exec command pid spawn exec child',
  },
  {
    title: 'std.env',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Environment variables: get, set, remove.',
    keywords: 'env environment get set path variable config',
  },
  {
    title: 'std.os',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Operating-system info: name, arch, cpuCount, home.',
    keywords: 'os system name arch cpu platform linux windows macos',
  },
  {
    title: 'std.net',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'HTTP client: get, post requests.',
    keywords: 'net http https get post client fetch request api',
  },
  {
    title: 'std.json',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'JSON parser and serializer: parse, stringify.',
    keywords: 'json parse stringify serialize deserialize data',
  },
  {
    title: 'std.testing',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Testing framework: begin, end, assertEqual, summary.',
    keywords: 'testing test assert equal summary suite unit',
  },
  {
    title: 'std.debug',
    category: 'StdLib',
    url: '/stdlib',
    desc: 'Debugging utilities: log, dump, time, timeEnd.',
    keywords: 'debug log dump time benchmark trace inspect',
  },

  // CLI
  {
    title: 'lamo run',
    category: 'CLI',
    url: '/cli',
    desc: 'Transpile to C, compile with GCC and execute immediately.',
    keywords: 'run compile gcc execute instant',
  },
  {
    title: 'lamo build',
    category: 'CLI',
    url: '/cli',
    desc: 'Compile to a native standalone binary without running it.',
    keywords: 'build binary executable compile -o output',
  },
  {
    title: 'lamo check',
    category: 'CLI',
    url: '/cli',
    desc: 'Parse and semantic type-check only — instantaneous feedback.',
    keywords: 'check lint verify syntax type check',
  },
  {
    title: 'lamo fmt',
    category: 'CLI',
    url: '/cli',
    desc: 'AST-based pretty-print in place with comment preservation.',
    keywords: 'fmt format pretty print indent style',
  },
  {
    title: 'lamo test',
    category: 'CLI',
    url: '/cli',
    desc: 'Run regression and unit test suites.',
    keywords: 'test regression suite runner',
  },
  {
    title: 'lamo repl',
    category: 'CLI',
    url: '/cli',
    desc: 'Interactive read-eval-print loop with tree-walking interpreter.',
    keywords: 'repl eval interactive terminal prompt',
  },
  {
    title: 'lampm init & install',
    category: 'CLI',
    url: '/cli',
    desc: 'Initialize projects (lamo.pkg) and install git dependencies.',
    keywords: 'lampm init install package pkg lock git dependencies',
  },
]

const categoryColors = {
  Guide: 'bg-blue-500/10 text-blue-600 dark:text-blue-400 border-blue-500/20',
  Docs: 'bg-purple-500/10 text-purple-600 dark:text-purple-400 border-purple-500/20',
  StdLib: 'bg-emerald-500/10 text-emerald-600 dark:text-emerald-400 border-emerald-500/20',
  CLI: 'bg-amber-500/10 text-amber-600 dark:text-amber-400 border-amber-500/20',
}

export default function SearchModal({ isOpen, onClose }) {
  const [query, setQuery] = useState('')
  const [selectedIndex, setSelectedIndex] = useState(0)
  const inputRef = useRef(null)
  const listRef = useRef(null)
  const navigate = useNavigate()

  useEffect(() => {
    if (isOpen) {
      setQuery('')
      setSelectedIndex(0)
      setTimeout(() => inputRef.current?.focus(), 50)
    }
  }, [isOpen])

  const filtered = query.trim()
    ? searchIndex.filter((item) => {
        const q = query.toLowerCase()
        return (
          item.title.toLowerCase().includes(q) ||
          item.desc.toLowerCase().includes(q) ||
          item.keywords.toLowerCase().includes(q)
        )
      })
    : searchIndex.slice(0, 8)

  useEffect(() => {
    setSelectedIndex(0)
  }, [query])

  const handleSelect = (item) => {
    onClose()
    if (item.url.includes('#')) {
      const [path, hash] = item.url.split('#')
      navigate(path)
      setTimeout(() => {
        const el = document.getElementById(hash)
        if (el) {
          el.scrollIntoView({ behavior: 'smooth', block: 'start' })
        }
      }, 50)
    } else {
      navigate(item.url)
    }
  }

  const handleKeyDown = (e) => {
    if (e.key === 'ArrowDown') {
      e.preventDefault()
      setSelectedIndex((prev) => (prev + 1) % Math.max(1, filtered.length))
    } else if (e.key === 'ArrowUp') {
      e.preventDefault()
      setSelectedIndex((prev) => (prev - 1 + filtered.length) % Math.max(1, filtered.length))
    } else if (e.key === 'Enter') {
      e.preventDefault()
      if (filtered[selectedIndex]) {
        handleSelect(filtered[selectedIndex])
      }
    } else if (e.key === 'Escape') {
      onClose()
    }
  }

  if (!isOpen) return null

  return (
    <div
      className="fixed inset-0 z-50 flex items-start justify-center p-4 sm:p-6 md:p-20 bg-black/60 backdrop-blur-sm transition-opacity"
      onClick={onClose}
    >
      <div
        className="relative w-full max-w-2xl overflow-hidden rounded-2xl border border-purple-500/25 bg-white shadow-2xl shadow-purple-950/40 dark:bg-[#120d24] dark:border-purple-500/30"
        onClick={(e) => e.stopPropagation()}
      >
        {/* Search Input Bar */}
        <div className="flex items-center border-b border-gray-200 dark:border-purple-900/40 px-4 py-3.5">
          <svg
            className="h-5 w-5 text-gray-400 dark:text-purple-400"
            fill="none"
            viewBox="0 0 24 24"
            strokeWidth="2"
            stroke="currentColor"
          >
            <path
              strokeLinecap="round"
              strokeLinejoin="round"
              d="M21 21l-5.197-5.197m0 0A7.5 7.5 0 105.196 5.196a7.5 7.5 0 0010.607 10.607z"
            />
          </svg>
          <input
            ref={inputRef}
            type="text"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="Search documentation, stdlib, CLI commands..."
            className="ml-3 flex-1 bg-transparent text-sm text-gray-900 placeholder-gray-400 focus:outline-none dark:text-white dark:placeholder-gray-500"
          />
          {query && (
            <button
              type="button"
              onClick={() => setQuery('')}
              className="rounded p-1 text-gray-400 hover:text-gray-600 dark:hover:text-gray-200"
            >
              <svg className="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
                <path
                  fillRule="evenodd"
                  d="M10 18a8 8 0 100-16 8 8 0 000 16zM8.707 7.293a1 1 0 00-1.414 1.414L8.586 10l-1.293 1.293a1 1 0 101.414 1.414L10 11.414l1.293 1.293a1 1 0 001.414-1.414L11.414 10l1.293-1.293a1 1 0 00-1.414-1.414L10 8.586 8.707 7.293z"
                  clipRule="evenodd"
                />
              </svg>
            </button>
          )}
          <kbd className="ml-2 hidden rounded border border-gray-200 bg-gray-100 px-1.5 py-0.5 text-[10px] font-medium text-gray-500 dark:border-purple-900/50 dark:bg-purple-950/60 dark:text-purple-300 sm:inline-block">
            ESC
          </kbd>
        </div>

        {/* Results List */}
        <div ref={listRef} className="max-h-96 overflow-y-auto p-2">
          {filtered.length === 0 ? (
            <div className="py-12 text-center">
              <p className="text-sm text-gray-500 dark:text-gray-400">No results found for &ldquo;{query}&rdquo;</p>
              <p className="mt-1 text-xs text-gray-400 dark:text-gray-500">
                Try searching for generics, traits, std.io, or lamo run.
              </p>
            </div>
          ) : (
            filtered.map((item, index) => {
              const isSelected = index === selectedIndex
              return (
                <div
                  key={item.title + item.url}
                  onClick={() => handleSelect(item)}
                  onMouseEnter={() => setSelectedIndex(index)}
                  className={`flex cursor-pointer items-start justify-between rounded-xl p-3 transition-colors ${
                    isSelected
                      ? 'bg-purple-50 dark:bg-purple-950/50 text-purple-950 dark:text-purple-100'
                      : 'hover:bg-gray-50 dark:hover:bg-purple-950/20 text-gray-700 dark:text-gray-300'
                  }`}
                >
                  <div className="min-w-0 flex-1 pr-3">
                    <div className="flex items-center gap-2">
                      <span className="font-semibold text-sm text-gray-900 dark:text-white">
                        {item.title}
                      </span>
                      <span
                        className={`rounded-md border px-1.5 py-0.5 text-[10px] font-medium ${
                          categoryColors[item.category] || 'border-gray-200 text-gray-500'
                        }`}
                      >
                        {item.category}
                      </span>
                    </div>
                    <p className="mt-0.5 text-xs text-gray-500 dark:text-gray-400 line-clamp-1">
                      {item.desc}
                    </p>
                  </div>
                  <svg
                    className={`h-4 w-4 shrink-0 transition-transform ${
                      isSelected ? 'text-purple-600 dark:text-purple-400 translate-x-0.5' : 'text-gray-400 opacity-0'
                    }`}
                    viewBox="0 0 20 20"
                    fill="currentColor"
                  >
                    <path
                      fillRule="evenodd"
                      d="M7.21 14.77a.75.75 0 01.02-1.06L11.168 10 7.23 6.29a.75.75 0 111.04-1.08l4.5 4.25a.75.75 0 010 1.08l-4.5 4.25a.75.75 0 01-1.06-.02z"
                      clipRule="evenodd"
                    />
                  </svg>
                </div>
              )
            })
          )}
        </div>

        {/* Modal Footer with Keyboard Shortcuts */}
        <div className="flex items-center justify-between border-t border-gray-200 bg-gray-50/70 px-4 py-2 text-xs text-gray-500 dark:border-purple-900/40 dark:bg-[#0c0819]/80 dark:text-gray-400">
          <div className="flex items-center gap-4">
            <span className="flex items-center gap-1">
              <kbd className="rounded border border-gray-200 bg-white px-1 py-0.5 text-[10px] dark:border-purple-900/50 dark:bg-purple-950/60">
                ↑
              </kbd>
              <kbd className="rounded border border-gray-200 bg-white px-1 py-0.5 text-[10px] dark:border-purple-900/50 dark:bg-purple-950/60">
                ↓
              </kbd>
              <span className="ml-1">to navigate</span>
            </span>
            <span className="flex items-center gap-1">
              <kbd className="rounded border border-gray-200 bg-white px-1 py-0.5 text-[10px] dark:border-purple-900/50 dark:bg-purple-950/60">
                ↵
              </kbd>
              <span className="ml-1">to select</span>
            </span>
          </div>
          <span>Search Lamo Docs</span>
        </div>
      </div>
    </div>
  )
}
