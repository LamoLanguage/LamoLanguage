import { useState } from 'react'
import Button from '../components/ui/Button.jsx'
import Badge from '../components/ui/Badge.jsx'
import Card from '../components/ui/Card.jsx'
import CodeBlock from '../components/CodeBlock.jsx'
import FaqAccordion from '../components/FaqAccordion.jsx'

const features = [
  {
    title: 'Hybrid Type Inference',
    badge: 'Core',
    body: 'Write `let x = 10` and let the compiler infer types, or annotate `let x: int = 10` for clarity. Obvious type errors like `"abc" * 3` are rejected at compile time.',
    icon: (
      <svg className="h-5 w-5" fill="none" viewBox="0 0 24 24" strokeWidth="1.5" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" d="M9.75 3.104v5.714a2.25 2.25 0 01-.659 1.591L5 14.5M9.75 3.104c-.251.023-.501.05-.75.082m.75-.082a24.301 24.301 0 014.5 0m0 0v5.714c0 .597.237 1.17.659 1.591L19.8 15.3M14.25 3.104c.251.023.501.05.75.082M19.8 15.3l-1.57.393A9.065 9.065 0 0112 15a9.065 9.065 0 00-6.23-.693L5 14.5m14.8.8l1.402 1.402c1.232 1.232.65 3.318-1.067 3.611A48.309 48.309 0 0112 21c-2.773 0-5.491-.235-8.135-.687-1.718-.293-2.3-2.379-1.067-3.61L5 14.5" />
      </svg>
    ),
  },
  {
    title: 'Parametric Generics',
    badge: '2.5.0',
    body: 'Generic structs, functions and typed arrays: `fn id<T>(x: T) -> T`, `struct Pair<A, B>`, `impl<T> Stack<T>`, with the `Any | Eq | Ord | Num | Hash | Show` constraint catalogue.',
    icon: (
      <svg className="h-5 w-5" fill="none" viewBox="0 0 24 24" strokeWidth="1.5" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" d="M21 7.5l-2.25-1.313M21 7.5v2.25m0-2.25l-2.25 1.313M3 7.5l2.25-1.313M3 7.5l2.25 1.313M3 7.5v2.25m9 3l2.25-1.313M12 12.75l-2.25-1.313M12 12.75V15m0 6.75l2.25-1.313M12 21.75V19.5m0 2.25l-2.25-1.313m0-16.5L12 2.25l2.25 1.313M21 14.25v2.25l-2.25 1.313m-13.5 0L3 16.5v-2.25" />
      </svg>
    ),
  },
  {
    title: 'Traits & Dictionaries',
    badge: '2.10.0',
    body: 'Declare compile-time contracts with `trait Shape { ... }` and implement them with `impl Shape for Circle`. Trait-constrained generics dispatch through generated dictionaries — no vtables.',
    icon: (
      <svg className="h-5 w-5" fill="none" viewBox="0 0 24 24" strokeWidth="1.5" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" d="M11.42 15.17L17.25 21A2.652 2.652 0 0021 17.25l-5.877-5.877M11.42 15.17l2.496-3.03c.317-.384.74-.626 1.208-.766M11.42 15.17l-4.655 5.653a2.548 2.548 0 11-3.586-3.586l6.837-5.63m5.108-.233c.55-.164 1.163-.188 1.743-.14a4.5 4.5 0 004.486-6.336l-3.276 3.277a3.004 3.004 0 01-2.25-2.25l3.276-3.276a4.5 4.5 0 00-6.336 4.486c.091 1.076-.071 2.264-.904 2.95l-.102.085m-1.745 1.437L5.909 7.5H4.5L2.25 3.75l1.5-1.5L7.5 4.5v1.409l4.26 4.26m-1.745 1.437l1.745-1.437m6.615 8.206L15.75 15.75M4.867 19.125h.008v.008h-.008v-.008z" />
      </svg>
    ),
  },
  {
    title: 'Native C Transpilation',
    badge: 'Toolchain',
    body: 'Lamo transpiles to clean, portable C and compiles with GCC. You get standalone native binaries, an embedded mark-sweep GC, and the entire C ecosystem.',
    icon: (
      <svg className="h-5 w-5" fill="none" viewBox="0 0 24 24" strokeWidth="1.5" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" d="M3.75 13.5l10.5-11.25L12 10.5h8.25L9.75 21.75 12 13.5H3.75z" />
      </svg>
    ),
  },
]

const heroSnippets = [
  {
    id: 'hello',
    label: 'hello.lamo',
    code: `fn main() {
    print("Hello, Lamo!");
    
    let items = ["speed", "safety", "simplicity"];
    for (let i = 0; i < items.len(); i++) {
        print("Feature: " + items[i]);
    }
}`,
  },
  {
    id: 'pattern',
    label: 'pattern_match.lamo',
    code: `enum Option<T> {
    Some(T),
    None,
}

fn describe<T: Show>(opt: Option<T>) -> string {
    match opt {
        Some(x) => "Value: " + x,
        None    => "Empty option",
    }
}

fn main() {
    let item = Some(42);
    print(describe(item));
}`,
  },
  {
    id: 'traits',
    label: 'traits.lamo',
    code: `trait Shape {
    fn area() -> float;
}

struct Circle { r: float }

impl Shape for Circle {
    fn area() -> float {
        return 3.14159 * self.r * self.r;
    }
}

fn print_area<T: Shape>(s: T) {
    print("Area: " + s.area());
}`,
  },
]

const quickStart = `# Build the compiler from source
git clone https://github.com/lamo-lang/lamo
cd lamo
make

# Run your first program
./lamo run hello.lamo`

const faqItems = [
  {
    question: 'What is Lamo?',
    answer:
      'Lamo is a small, modern programming language that transpiles to C. It pairs a friendly, Python-like surface syntax with compile-time type checking and native performance through GCC.',
  },
  {
    question: 'Is Lamo statically or dynamically typed?',
    answer:
      'Both — Lamo uses hybrid type inference. Annotations are optional on locals and functions but mandatory on struct fields. The semantic pass infers what it can and rejects clear type errors at compile time.',
  },
  {
    question: 'How do I manage memory?',
    answer:
      'You do not. Lamo values live behind a mark-sweep garbage collector built into the runtime. Strings are immutable and arena-allocated; arrays and structs are reference-counted shared objects in the interpreter.',
  },
  {
    question: 'Does Lamo have a package manager?',
    answer:
      'Yes — lampm ships inside the lamo binary. Use lamo init, lamo install, lamo update, lamo remove, lamo list, lamo info and lamo lock to manage dependencies declared in lamo.pkg and pinned in lamo.lock.',
  },
]

export default function Home() {
  const [activeTab, setActiveTab] = useState(0)

  return (
    <div className="overflow-hidden">
      {/* Hero Section */}
      <section className="relative overflow-hidden border-b border-gray-200/80 bg-gradient-to-b from-purple-50/40 via-white to-white py-20 dark:border-purple-900/30 dark:from-[#140c2b] dark:via-[#0c0819] dark:to-[#0a0714] sm:py-28">
        {/* Ambient Purple Glow Elements */}
        <div className="pointer-events-none absolute inset-0 bg-grid-pattern opacity-50 dark:opacity-40" />
        <div className="pointer-events-none absolute -top-40 left-1/2 -translate-x-1/2 h-[500px] w-[800px] rounded-full bg-purple-500/15 blur-[120px] dark:bg-purple-600/20" />

        <div className="relative mx-auto flex max-w-7xl flex-col items-center px-4 text-center sm:px-6 lg:px-8">
          {/* Logo & Version Pill */}
          <div className="mb-6 flex items-center gap-3">
            <img
              src="/icon.jpg"
              alt="Lamo logo"
              className="h-16 w-16 rounded-2xl shadow-md ring-1 ring-purple-500/30 sm:h-20 sm:w-20"
            />
          </div>

          <Badge color="brand" size="md" dot={true}>
            v2.10.0 Released — Dictionary Dispatch &amp; Traits
          </Badge>

          {/* Heading */}
          <h1 className="mt-6 max-w-4xl text-4xl font-extrabold tracking-tight text-gray-950 dark:text-white sm:text-6xl sm:leading-[1.1]">
            A modern programming language that{' '}
            <span className="bg-gradient-to-r from-purple-600 via-brand-500 to-indigo-600 bg-clip-text text-transparent dark:from-purple-400 dark:via-purple-300 dark:to-indigo-300">
              transpiles to C
            </span>
          </h1>

          <p className="mt-6 max-w-2xl text-lg leading-8 text-gray-600 dark:text-gray-300">
            Lamo combines Python-like ergonomics, hybrid type inference, generics and traits
            with the raw speed of compiled C. Small enough to learn in a weekend, real enough
            to ship.
          </p>

          {/* Dual CTAs */}
          <div className="mt-8 flex flex-wrap items-center justify-center gap-3.5">
            <Button to="/getting-started" size="lg" className="shadow-lg shadow-brand-500/25">
              Get Started
              <svg className="h-4 w-4 ml-0.5" viewBox="0 0 20 20" fill="currentColor">
                <path fillRule="evenodd" d="M7.21 14.77a.75.75 0 01.02-1.06L11.168 10 7.23 6.29a.75.75 0 111.04-1.08l4.5 4.25a.75.75 0 010 1.08l-4.5 4.25a.75.75 0 01-1.06-.02z" clipRule="evenodd" />
              </svg>
            </Button>
            <Button
              to="/docs"
              variant="secondary"
              size="lg"
              className="border-purple-500/30 hover:border-purple-500/60 hover:shadow-glow-sm"
            >
              Read the Docs
            </Button>
            <a
              href="https://github.com/lamo-lang/lamo"
              target="_blank"
              rel="noopener noreferrer"
              className="inline-flex items-center gap-2 rounded-lg border border-gray-200 bg-white/80 px-5 py-2.5 text-base font-medium text-gray-700 hover:bg-gray-100/80 hover:text-purple-600 dark:border-purple-900/40 dark:bg-purple-950/30 dark:text-purple-200 dark:hover:bg-purple-900/40 transition-colors"
            >
              <svg className="h-4 w-4" viewBox="0 0 24 24" fill="currentColor">
                <path fillRule="evenodd" d="M12 2C6.477 2 2 6.484 2 12.017c0 4.425 2.865 8.18 6.839 9.504.5.092.682-.217.682-.483 0-.237-.008-.868-.013-1.703-2.782.605-3.369-1.343-3.369-1.343-.454-1.158-1.11-1.466-1.11-1.466-.908-.62.069-.608.069-.608 1.003.07 1.531 1.032 1.531 1.032.892 1.53 2.341 1.088 2.91.832.092-.647.35-1.088.636-1.338-2.22-.253-4.555-1.113-4.555-4.951 0-1.093.39-1.988 1.029-2.688-.103-.253-.446-1.272.098-2.65 0 0 .84-.27 2.75 1.026A9.564 9.564 0 0112 6.844c.85.004 1.705.115 2.504.337 1.909-1.296 2.747-1.027 2.747-1.027.546 1.379.202 2.398.1 2.651.64.7 1.028 1.595 1.028 2.688 0 3.848-2.339 4.695-4.566 4.943.359.309.678.92.678 1.855 0 1.338-.012 2.419-.012 2.747 0 .268.18.58.688.482A10.019 10.019 0 0022 12.017C22 6.484 17.522 2 12 2z" clipRule="evenodd" />
              </svg>
              GitHub
            </a>
          </div>

          {/* Interactive Code Preview Box */}
          <div className="mt-14 w-full max-w-3xl text-left">
            <div className="flex items-center justify-between px-2 pb-2">
              <div className="flex gap-2">
                {heroSnippets.map((snippet, idx) => (
                  <button
                    key={snippet.id}
                    type="button"
                    onClick={() => setActiveTab(idx)}
                    className={`rounded-lg px-3 py-1.5 font-mono text-xs font-medium transition-all ${
                      activeTab === idx
                        ? 'bg-purple-600/15 text-purple-700 dark:bg-purple-500/20 dark:text-purple-300 ring-1 ring-inset ring-purple-500/30'
                        : 'text-gray-500 hover:text-gray-900 dark:text-gray-400 dark:hover:text-purple-200'
                    }`}
                  >
                    {snippet.label}
                  </button>
                ))}
              </div>
              <span className="hidden text-xs text-gray-400 dark:text-purple-400/60 sm:inline-block font-mono">
                transpiled to C
              </span>
            </div>
            <CodeBlock
              code={heroSnippets[activeTab].code}
              title={heroSnippets[activeTab].label}
              lang="lamo"
            />
          </div>
        </div>
      </section>

      {/* Features Grid */}
      <section className="mx-auto max-w-7xl px-4 py-20 sm:px-6 lg:px-8">
        <div className="mx-auto max-w-2xl text-center">
          <Badge color="brand" dot={true}>Features</Badge>
          <h2 className="mt-3 text-3xl font-bold tracking-tight text-gray-950 dark:text-white sm:text-4xl">
            Small language, sharp edges rounded
          </h2>
          <p className="mt-4 text-lg text-gray-600 dark:text-gray-300">
            Everything in Lamo exists because it earned its place in the formal language spec.
          </p>
        </div>
        <div className="mt-12 grid gap-6 sm:grid-cols-2 lg:grid-cols-4">
          {features.map((f) => (
            <Card key={f.title} icon={f.icon}>
              <div className="mb-2 flex items-center justify-between gap-2">
                <h3 className="text-base font-semibold text-gray-900 dark:text-white">{f.title}</h3>
                <Badge size="sm" color="brand">{f.badge}</Badge>
              </div>
              <p className="text-sm leading-6 text-gray-600 dark:text-gray-300">{f.body}</p>
            </Card>
          ))}
        </div>
      </section>

      {/* Quick Start Section */}
      <section className="border-y border-gray-200/80 bg-gray-50/70 dark:border-purple-900/30 dark:bg-[#0d091d]/60 py-16">
        <div className="mx-auto grid max-w-7xl gap-10 px-4 sm:px-6 lg:grid-cols-2 lg:px-8 items-center">
          <div>
            <Badge color="brand" dot={true}>Quick Start</Badge>
            <h2 className="mt-3 text-3xl font-bold tracking-tight text-gray-950 dark:text-white">
              From zero to running in 60 seconds
            </h2>
            <p className="mt-4 text-base text-gray-600 dark:text-gray-300 leading-relaxed">
              Build the compiler with <code className="rounded bg-gray-200 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm dark:text-purple-300">make</code>,
              write a <span className="font-mono text-sm text-purple-600 dark:text-purple-300">.lamo</span> file, and run it. The
              compiler translates to C and calls GCC behind the scenes.
            </p>
            <div className="mt-6">
              <CodeBlock code={quickStart} lang="bash" title="terminal" />
            </div>
          </div>
          <div className="flex flex-col justify-center">
            <CodeBlock
              code={`fn main() {\n    print("Hello, Lamo!");\n}`}
              title="hello.lamo"
            />
            <div className="mt-2">
              <CodeBlock
                code={`./lamo run hello.lamo\n# Hello, Lamo!`}
                lang="bash"
                title="output"
              />
            </div>
          </div>
        </div>
      </section>

      {/* FAQ Section */}
      <section className="mx-auto max-w-3xl px-4 py-20 sm:px-6 lg:px-8">
        <div className="text-center">
          <Badge color="brand" dot={true}>FAQ</Badge>
          <h2 className="mt-3 text-3xl font-bold tracking-tight text-gray-950 dark:text-white">
            Frequently asked questions
          </h2>
        </div>
        <div className="mt-10">
          <FaqAccordion items={faqItems} />
        </div>

        {/* Newsletter Box */}
        <div className="mt-14 rounded-2xl border border-purple-500/20 bg-gradient-to-br from-purple-50/50 via-white to-purple-50/30 p-8 shadow-sm dark:from-[#140e29] dark:via-[#100b21] dark:to-[#0d091b] dark:border-purple-500/20">
          <h3 className="text-base font-semibold text-gray-950 dark:text-white">
            Stay in the loop
          </h3>
          <p className="mt-1 text-sm text-gray-600 dark:text-gray-300">
            Get compiler release notes, new stdlib modules and RFC announcements in your inbox.
          </p>
          <form
            className="mt-4 flex flex-col sm:flex-row gap-2.5"
            onSubmit={(e) => {
              e.preventDefault()
              alert('Thank you for subscribing!')
            }}
          >
            <input
              type="email"
              placeholder="you@example.com"
              required
              className="flex-1 rounded-lg border border-gray-300 bg-white px-3.5 py-2 text-sm text-gray-900 placeholder-gray-400 focus:border-purple-500 focus:outline-none focus:ring-2 focus:ring-purple-500/20 dark:border-purple-900/50 dark:bg-[#191238] dark:text-white dark:placeholder-gray-500"
            />
            <Button type="submit" size="md">
              Subscribe
            </Button>
          </form>
        </div>
      </section>
    </div>
  )
}
