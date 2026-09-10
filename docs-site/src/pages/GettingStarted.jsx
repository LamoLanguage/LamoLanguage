import DocsLayout from '../components/DocsLayout.jsx'
import CodeBlock from '../components/CodeBlock.jsx'
import Alert from '../components/ui/Alert.jsx'
import Badge from '../components/ui/Badge.jsx'
import Button from '../components/ui/Button.jsx'

const helloWorld = `fn main() {
    print("Hello, Lamo!");
}`

export default function GettingStarted() {
  return (
    <DocsLayout>
      <div className="max-w-3xl">
        <Badge color="brand" dot={true}>Guide</Badge>
        <h1 className="mt-3 text-4xl font-extrabold tracking-tight text-gray-950 dark:text-white">
          Getting Started
        </h1>
        <p className="mt-4 text-lg leading-8 text-gray-600 dark:text-gray-300">
          Lamo ships as a single, self-contained compiler binary. Build it from source, then use the
          <code className="mx-1.5 rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">lamo</code>
          CLI to compile, check, format, and run your programs.
        </p>

        <section className="mt-12">
          <h2 className="text-2xl font-bold text-gray-900 dark:text-white">1. Prerequisites</h2>
          <p className="mt-3 text-gray-600 dark:text-gray-300">
            You need a standard C toolchain: <strong>GCC</strong> (or Clang), <strong>GNU Make</strong>,
            and <strong>Python 3</strong> (used to embed the runtime header into C during the build).
          </p>
          <div className="mt-4">
            <Alert type="warning" title="Windows Users">
              The build targets POSIX environments. On Windows, build inside MSYS2/MinGW,
              Cygwin or WSL, then run the produced <code className="font-mono text-xs">lamo.exe</code>.
            </Alert>
          </div>
        </section>

        <section className="mt-12">
          <h2 className="text-2xl font-bold text-gray-900 dark:text-white">2. Build from source</h2>
          <div className="mt-4">
            <CodeBlock
              code={`git clone https://github.com/lamo-lang/lamo\ncd lamo\nmake            # produces ./lamo\nmake test       # optional: run the regression suite`}
              lang="bash"
              title="terminal"
            />
          </div>
          <p className="mt-4 text-gray-600 dark:text-gray-300">
            Verify the compiler installation:
          </p>
          <div className="mt-4">
            <CodeBlock code={`./lamo version\n# lamo 2.10.0`} lang="bash" title="terminal" />
          </div>
        </section>

        <section className="mt-12">
          <h2 className="text-2xl font-bold text-gray-900 dark:text-white">3. Your first program</h2>
          <p className="mt-3 text-gray-600 dark:text-gray-300">
            Create a file named <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">hello.lamo</code>:
          </p>
          <div className="mt-4">
            <CodeBlock code={helloWorld} title="hello.lamo" />
          </div>
          <p className="mt-4 text-gray-600 dark:text-gray-300">
            Run it — Lamo transpiles to C, compiles with GCC and executes immediately:
          </p>
          <div className="mt-4">
            <CodeBlock code={`./lamo run hello.lamo\n# Hello, Lamo!`} lang="bash" title="terminal" />
          </div>
        </section>

        <section className="mt-12">
          <h2 className="text-2xl font-bold text-gray-900 dark:text-white">4. Everyday commands</h2>
          <div className="mt-4">
            <CodeBlock
              code={`./lamo check hello.lamo   # parse + type-check only (instant)\n./lamo build hello.lamo -o hello   # emit a native standalone binary\n./lamo fmt hello.lamo        # canonical formatting (AST-based)\n./lamo test                  # run test suite\n./lamo repl                  # interactive interpreter`}
              lang="bash"
              title="terminal"
            />
          </div>
          <p className="mt-4 text-gray-600 dark:text-gray-300">
            See the{' '}
            <a href="/cli" className="font-medium text-purple-600 hover:text-purple-700 dark:text-purple-400 dark:hover:text-purple-300 underline underline-offset-2">
              CLI reference
            </a>{' '}
            for full flags and the built-in lampm package manager.
          </p>
        </section>

        <section className="mt-12">
          <h2 className="text-2xl font-bold text-gray-900 dark:text-white">5. Create a project with lampm</h2>
          <div className="mt-4">
            <CodeBlock
              code={`lamo init my-app      # scaffold lamo.pkg\ncd my-app\nlamo install           # resolve + install dependencies\nlamo run src/main.lamo`}
              lang="bash"
              title="terminal"
            />
          </div>
          <div className="mt-4">
            <Alert type="info" title="Dependencies">
              lampm reads <code className="font-mono">lamo.pkg</code>, resolves git-based
              dependencies and pins exact versions in <code className="font-mono">lamo.lock</code>.
              Run <code className="font-mono">lamo doctor</code> to diagnose toolchain issues.
            </Alert>
          </div>
        </section>

        <div className="mt-16 flex flex-wrap gap-3.5 border-t border-gray-200 dark:border-purple-900/30 pt-8">
          <Button to="/docs">Continue: Language Guide</Button>
          <Button to="/stdlib" variant="secondary">
            Explore the Stdlib
          </Button>
        </div>
      </div>
    </DocsLayout>
  )
}
