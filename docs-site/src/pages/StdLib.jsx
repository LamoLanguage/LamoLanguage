import { useState, useMemo } from 'react'
import DocsLayout from '../components/DocsLayout.jsx'
import CodeBlock from '../components/CodeBlock.jsx'
import Badge from '../components/ui/Badge.jsx'
import Alert from '../components/ui/Alert.jsx'

const modules = [
  {
    name: 'io',
    category: 'I/O',
    tagline: 'Console input/output operations',
    api: 'println, eprint, read_line, write',
    example: `import std.io;\nio.println("Hello, stdout!");\nio.eprint("Warning to stderr");`,
  },
  {
    name: 'fs',
    category: 'I/O',
    tagline: 'Synchronous file system operations',
    api: 'readText, writeText, exists, remove',
    example: `import std.fs;\nif (fs.exists("config.txt")) {\n    print(fs.readText("config.txt"));\n}`,
  },
  {
    name: 'path',
    category: 'I/O',
    tagline: 'Cross-platform path manipulation',
    api: 'join, parent, filename, normalize',
    example: `import std.path;\nlet full = path.join("src", "main.lamo");\nprint(path.filename(full)); // main.lamo`,
  },
  {
    name: 'string',
    category: 'Data',
    tagline: 'High-performance string utilities',
    api: 'length, split, trim, replace, upper, lower',
    example: `import std.string;\nprint(string.upper("lamo"));   // LAMO\nlet parts = string.split("a,b,c", ",");`,
  },
  {
    name: 'math',
    category: 'Data',
    tagline: 'Mathematical functions and constants',
    api: 'sqrt, pow, sin, cos, PI, floor, ceil',
    example: `import std.math;\nprint(math.sqrt(25));   // 5\nprint(math.PI);         // 3.1415926535...`,
  },
  {
    name: 'random',
    category: 'Data',
    tagline: 'Pseudo-random generation & shuffling',
    api: 'int, float, bool, choice, shuffle',
    example: `import std.random;\nlet dice = random.int(1, 6);\nlet flag = random.bool();`,
  },
  {
    name: 'time',
    category: 'System',
    tagline: 'Clock, sleep and monotonic timers',
    api: 'now, sleep, monotonic',
    example: `import std.time;\nlet t0 = time.monotonic();\ntime.sleep(50);   // milliseconds\nprint("Elapsed: " + (time.monotonic() - t0));`,
  },
  {
    name: 'collections',
    category: 'Data',
    tagline: 'Parametric data structures',
    api: 'List, Stack, Queue, HashMap, HashSet',
    example: `import std.collections;\nlet m = collections.hashMapNew();\ncollections.mapSet(m, "key", 42);`,
    generic: true,
  },
  {
    name: 'process',
    category: 'System',
    tagline: 'Process execution and environment info',
    api: 'run, exec, currentPid',
    example: `import std.process;\nprint("PID: " + process.currentPid());`,
  },
  {
    name: 'env',
    category: 'System',
    tagline: 'Environment variable access',
    api: 'get, set, remove',
    example: `import std.env;\nlet home = env.get("HOME");\nprint("Home: " + home);`,
  },
  {
    name: 'os',
    category: 'System',
    tagline: 'Operating system and architecture detection',
    api: 'name, arch, cpuCount, home',
    example: `import std.os;\nprint("OS: " + os.name());       // windows | linux | macos\nprint("CPUs: " + os.cpuCount());`,
  },
  {
    name: 'net',
    category: 'Network',
    tagline: 'HTTP request client',
    api: 'get, post',
    example: `import std.net;\nlet res = net.get("https://httpbin.org/get");\nprint(res);`,
  },
  {
    name: 'json',
    category: 'Data',
    tagline: 'Fast JSON serialization & parser',
    api: 'parse, stringify',
    example: `import std.json;\nlet parsed = json.parse("{\\"status\\": 200}");\nprint(json.stringify(parsed));`,
  },
  {
    name: 'testing',
    category: 'Utility',
    tagline: 'Unit testing harness and assertion tools',
    api: 'begin, end, assertEqual, summary',
    example: `import std.testing;\ntesting.begin("arithmetic");\ntesting.assertEqual(2 + 2, 4);\ntesting.end();\ntesting.summary();`,
  },
  {
    name: 'debug',
    category: 'Utility',
    tagline: 'Diagnostics, benchmarking and object dumps',
    api: 'log, dump, time, timeEnd',
    example: `import std.debug;\ndebug.time("sort-task");\n// ... computation ...\ndebug.timeEnd("sort-task");`,
  },
]

const categories = ['All', 'I/O', 'System', 'Data', 'Network', 'Utility']

export default function StdLib() {
  const [selectedCategory, setSelectedCategory] = useState('All')
  const [searchQuery, setSearchQuery] = useState('')

  const filteredModules = useMemo(() => {
    return modules.filter((m) => {
      const matchesCategory =
        selectedCategory === 'All' || m.category === selectedCategory
      const q = searchQuery.toLowerCase().trim()
      const matchesSearch =
        !q ||
        m.name.toLowerCase().includes(q) ||
        m.tagline.toLowerCase().includes(q) ||
        m.api.toLowerCase().includes(q)
      return matchesCategory && matchesSearch
    })
  }, [selectedCategory, searchQuery])

  return (
    <DocsLayout>
      <div className="max-w-4xl">
        <Badge color="brand" dot={true}>Standard Library</Badge>
        <h1 className="mt-3 text-4xl font-extrabold tracking-tight text-gray-950 dark:text-white">
          15 Modules, Zero External Dependencies
        </h1>
        <p className="mt-4 text-lg leading-8 text-gray-600 dark:text-gray-300">
          The standard library ships built-in alongside the compiler. Every module is available via{' '}
          <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">
            import std.&lt;module&gt;
          </code>
          . High-throughput paths are backed by C runtime primitives; the remainder is pure, readable Lamo.
        </p>

        <div className="mt-6">
          <CodeBlock
            code={`import std.io;\nimport std.math as math;\n\nio.println("sqrt(16) = " + math.sqrt(16));`}
            title="imports.lamo"
          />
        </div>

        <div className="mt-6">
          <Alert type="info" title="Namespaced Imports">
            <code className="font-mono">import std.math;</code> binds the alias{' '}
            <code className="font-mono font-semibold">math</code> (the last path component by default).
            Only <code className="font-mono font-semibold">pub</code> members are accessible through an alias.
          </Alert>
        </div>

        {/* Filter and Search Controls */}
        <div className="mt-10 space-y-4">
          <div className="flex flex-col sm:flex-row gap-3 items-stretch sm:items-center justify-between">
            {/* Category Filter Pills */}
            <div className="flex flex-wrap gap-1.5">
              {categories.map((cat) => (
                <button
                  key={cat}
                  type="button"
                  onClick={() => setSelectedCategory(cat)}
                  className={`rounded-lg px-3 py-1.5 text-xs font-medium transition-all ${
                    selectedCategory === cat
                      ? 'bg-purple-600 text-white shadow-sm shadow-purple-500/25 dark:bg-purple-500'
                      : 'bg-gray-100 text-gray-600 hover:bg-gray-200/80 dark:bg-[#181230] dark:text-gray-300 dark:hover:bg-[#221a42]'
                  }`}
                >
                  {cat}
                </button>
              ))}
            </div>

            {/* Live Search Input */}
            <div className="relative min-w-[240px]">
              <input
                type="text"
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
                placeholder="Filter by name or function..."
                className="w-full rounded-lg border border-gray-300 bg-white px-3.5 py-1.5 text-xs text-gray-900 placeholder-gray-400 focus:border-purple-500 focus:outline-none focus:ring-2 focus:ring-purple-500/20 dark:border-purple-900/50 dark:bg-[#140e29] dark:text-white dark:placeholder-gray-500"
              />
              {searchQuery && (
                <button
                  type="button"
                  onClick={() => setSearchQuery('')}
                  className="absolute right-2.5 top-1/2 -translate-y-1/2 text-gray-400 hover:text-gray-600 dark:hover:text-gray-200 text-xs"
                >
                  ✕
                </button>
              )}
            </div>
          </div>

          <p className="text-xs text-gray-500 dark:text-gray-400">
            Showing {filteredModules.length} of {modules.length} modules
          </p>
        </div>

        {/* Modules Grid */}
        <div className="mt-6 grid gap-5 sm:grid-cols-2">
          {filteredModules.map((m) => (
            <div
              key={m.name}
              className="group rounded-xl border border-gray-200/80 bg-white p-5 shadow-sm transition-all hover:border-purple-400/50 hover:shadow-md dark:border-purple-500/20 dark:bg-[#120d24]/90 dark:backdrop-blur-sm dark:hover:border-purple-500/40"
            >
              <div className="flex items-center justify-between gap-2">
                <div className="flex items-center gap-2">
                  <h2 className="font-mono text-base font-bold text-gray-950 dark:text-white group-hover:text-purple-600 dark:group-hover:text-purple-400 transition-colors">
                    std.{m.name}
                  </h2>
                  <span className="rounded-md border border-purple-500/20 bg-purple-500/10 px-1.5 py-0.5 text-[10px] font-medium text-purple-700 dark:text-purple-300">
                    {m.category}
                  </span>
                </div>
                {m.generic && (
                  <Badge size="sm" color="green">
                    generic
                  </Badge>
                )}
              </div>
              <p className="mt-1.5 text-sm text-gray-600 dark:text-gray-300">{m.tagline}</p>
              <p className="mt-2 text-xs text-gray-500 dark:text-gray-400">
                <span className="font-medium text-gray-700 dark:text-gray-300">API:</span>{' '}
                <code className="font-mono text-[11px] text-purple-700 dark:text-purple-300">{m.api}</code>
              </p>
              <div className="mt-3">
                <CodeBlock code={m.example} />
              </div>
            </div>
          ))}
        </div>

        <p className="mt-12 text-sm text-gray-500 dark:text-gray-400">
          Every standard module ships an API contract, runnable test cases under{' '}
          <code className="font-mono text-purple-600 dark:text-purple-400">std/tests/</code> and runnable examples under{' '}
          <code className="font-mono text-purple-600 dark:text-purple-400">std/examples/</code> in the Lamo repository.
        </p>
      </div>
    </DocsLayout>
  )
}
