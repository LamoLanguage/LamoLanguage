import DocsLayout from '../components/DocsLayout.jsx'
import CodeBlock from '../components/CodeBlock.jsx'
import Badge from '../components/ui/Badge.jsx'
import Alert from '../components/ui/Alert.jsx'

const commands = [
  { cmd: 'lamo run <file.lamo>', desc: 'Transpile to C, compile with GCC and execute immediately.' },
  { cmd: 'lamo build <file.lamo> -o <out>', desc: 'Compile to a native binary without running it.' },
  { cmd: 'lamo check <file.lamo>', desc: 'Parse and semantic-check only — instantaneous feedback, no codegen.' },
  { cmd: 'lamo fmt <file.lamo>', desc: 'AST-based pretty-print in place. Never corrupts unparseable files.' },
  { cmd: 'lamo test', desc: 'Run the project test suite (tests/run_tests.sh).' },
  { cmd: 'lamo eval <file.lamo>', desc: 'Tree-walking interpreter; byte-for-byte parity with compiled output.' },
  { cmd: 'lamo repl', desc: 'Interactive read-eval-print loop backed by the interpreter.' },
  { cmd: 'lamo help', desc: 'Show CLI usage, or per-command help with lamo help <command>.' },
  { cmd: 'lamo version', desc: 'Print compiler version and target information.' },
]

const lampmCommands = [
  { cmd: 'lamo init [name]', desc: 'Scaffold a new lamo.pkg project manifest.' },
  { cmd: 'lamo install [dep]', desc: 'Resolve and install dependencies from lamo.pkg.' },
  { cmd: 'lamo update [dep]', desc: 'Update one or all dependencies within specified version bounds.' },
  { cmd: 'lamo remove <dep>', desc: 'Remove a dependency from manifest and purge cached files.' },
  { cmd: 'lamo list', desc: 'List installed dependencies and their resolved versions.' },
  { cmd: 'lamo info <dep>', desc: 'Show detailed metadata about an installed package.' },
  { cmd: 'lamo lock', desc: 'Regenerate lamo.lock, pinning exact resolved git commits.' },
  { cmd: 'lamo cache', desc: 'Inspect or clear the global ~/.lamo/cache repository cache.' },
  { cmd: 'lamo doctor', desc: 'Diagnose compiler environment and C toolchain prerequisites.' },
]

function CommandTable({ rows }) {
  return (
    <div className="overflow-hidden rounded-xl border border-gray-200/80 dark:border-purple-900/40 shadow-sm">
      <table className="min-w-full divide-y divide-gray-200 dark:divide-purple-900/30">
        <tbody className="divide-y divide-gray-200 dark:divide-purple-900/30 bg-white dark:bg-[#120d24]">
          {rows.map((r) => (
            <tr key={r.cmd} className="hover:bg-purple-50/40 dark:hover:bg-purple-950/20 transition-colors">
              <td className="whitespace-nowrap px-4 py-3 align-top">
                <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-2 py-1 font-mono text-xs font-semibold text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">
                  {r.cmd}
                </code>
              </td>
              <td className="px-4 py-3 text-sm text-gray-600 dark:text-gray-300">{r.desc}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

export default function Cli() {
  return (
    <DocsLayout>
      <div className="max-w-4xl">
        <Badge color="brand" dot={true}>Reference</Badge>
        <h1 className="mt-3 text-4xl font-extrabold tracking-tight text-gray-950 dark:text-white">
          CLI &amp; Toolchain Reference
        </h1>
        <p className="mt-4 text-lg leading-8 text-gray-600 dark:text-gray-300">
          <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">lamo</code> is a
          single unified binary: compiler, interpreter, formatter and the integrated lampm package manager.
        </p>

        <section className="mt-12">
          <h2 className="text-2xl font-bold text-gray-950 dark:text-white">Compiler Commands</h2>
          <div className="mt-4">
            <CommandTable rows={commands} />
          </div>
          <div className="mt-6">
            <CodeBlock
              code={`lamo run hello.lamo --verbose\nlamo check src/main.lamo --no-color\nLAMO_NO_COLOR=1 lamo check src/main.lamo`}
              lang="bash"
              title="global flags"
            />
          </div>
          <p className="mt-4 text-sm text-gray-600 dark:text-gray-300">
            Global flags: <code className="rounded bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-xs text-purple-700 dark:text-purple-300">--verbose</code>,{' '}
            <code className="rounded bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-xs text-purple-700 dark:text-purple-300">--quiet</code>,{' '}
            <code className="rounded bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-xs text-purple-700 dark:text-purple-300">--no-color</code>.
            Diagnostics format as <code className="font-mono text-xs text-purple-600 dark:text-purple-400">file:line:col</code> with caret
            source excerpts and actionable hints.
          </p>
        </section>

        <section className="mt-14">
          <div className="flex items-center gap-2.5">
            <h2 className="text-2xl font-bold text-gray-950 dark:text-white">lampm — Package Manager</h2>
            <Badge color="green" size="sm">
              built-in
            </Badge>
          </div>
          <p className="mt-3 text-gray-600 dark:text-gray-300">
            lampm is compiled directly into the lamo binary. It manages{' '}
            <code className="rounded bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300">lamo.pkg</code>{' '}
            manifests, git-based dependencies and the reproducible{' '}
            <code className="rounded bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300">lamo.lock</code>{' '}
            lockfile.
          </p>
          <div className="mt-4">
            <CommandTable rows={lampmCommands} />
          </div>
          <div className="mt-6">
            <CodeBlock
              code={`lamo init my-app\ncd my-app\nlamo install\nlamo run src/main.lamo`}
              lang="bash"
              title="new project"
            />
          </div>
          <div className="mt-6">
            <Alert type="info" title="Deterministic Builds">
              Always commit <code className="font-mono">lamo.lock</code>. The{' '}
              <code className="font-mono">lamo install</code> command honors the lockfile, ensuring every machine
              and CI runner resolves the exact same git commits.
            </Alert>
          </div>
        </section>
      </div>
    </DocsLayout>
  )
}
