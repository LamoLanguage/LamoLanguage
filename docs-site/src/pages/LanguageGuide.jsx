import DocsLayout from '../components/DocsLayout.jsx'
import CodeBlock from '../components/CodeBlock.jsx'
import Alert from '../components/ui/Alert.jsx'
import Badge from '../components/ui/Badge.jsx'

function Section({ id, title, children }) {
  return (
    <section id={id} className="scroll-mt-24 border-b border-gray-200/80 dark:border-purple-900/30 pb-12 pt-12 first:pt-0">
      <div className="flex items-center gap-2 group">
        <h2 className="text-2xl font-bold tracking-tight text-gray-950 dark:text-white">
          {title}
        </h2>
        <a
          href={`#${id}`}
          className="opacity-0 group-hover:opacity-100 text-purple-500 hover:text-purple-600 transition-opacity"
          aria-label={`Link to ${title}`}
        >
          #
        </a>
      </div>
      <div className="mt-4 space-y-4 text-gray-600 dark:text-gray-300 leading-relaxed">{children}</div>
    </section>
  )
}

export default function LanguageGuide() {
  return (
    <DocsLayout showToc={true}>
      <div className="max-w-3xl">
        <Badge color="brand" dot={true}>Language Guide</Badge>
        <h1 className="mt-3 text-4xl font-extrabold tracking-tight text-gray-950 dark:text-white">
          The Lamo Language
        </h1>
        <p className="mt-4 text-lg leading-8 text-gray-600 dark:text-gray-300">
          A tour of the core language: variables, control flow, functions, structs, enums with
          pattern matching, generics and traits. Condensed from the authoritative{' '}
          <span className="font-mono text-sm text-purple-600 dark:text-purple-300">SPEC.md</span>.
        </p>

        <Section id="variables" title="Variables & Types">
          <p>
            Declare variables with <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">let</code>.
            The type system is <em>hybrid</em>: annotations are optional on locals and
            functions — inference fills the gaps — while struct fields always carry explicit types.
          </p>
          <CodeBlock
            code={`let x = 10;            // int, inferred\nlet pi: float = 3.14;  // annotated and checked\nlet name = "Lamo";     // string\nlet xs = [1, 2, 3];    // array; xs[-1] is 3\nxs.push(4);            // arrays grow dynamically\n\n// Builtin predicates\nprint(isnumber(x));    // true\nprint(isstring(name)); // true`}
            title="variables.lamo"
          />
          <p>
            Scalar types are <Badge size="sm" color="brand">int</Badge>{' '}
            <Badge size="sm" color="brand">float</Badge>{' '}
            <Badge size="sm" color="brand">string</Badge>{' '}
            <Badge size="sm" color="brand">bool</Badge>{' '}
            <Badge size="sm" color="brand">void</Badge>, plus{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">array&lt;T&gt;</code>,
            structs and enums. Strings are immutable and arena-backed. Obvious misuse like{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">"abc" * 3</code>{' '}
            is rejected at compile time.
          </p>
        </Section>

        <Section id="control-flow" title="Control Flow">
          <p>
            Supports <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">if/else</code>,{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">while</code>, and{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">for</code> loops with{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">break</code> and{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">continue</code>.
            Conditions are <strong>Python-like</strong>, not strict-bool.
          </p>
          <CodeBlock
            code={`if (5) {           // truthy: any non-zero number\n    print("runs");\n}\nif ("") {          // falsy: "", 0, 0.0\n    print("never runs");\n}\n\nfor (let i = 0; i < 10; i++) {\n    if (i == 3) { continue; }\n    if (i == 7) { break; }\n    print(i);\n}\n\nlet n = 0;\nwhile (n < 3) {\n    n += 1;\n}`}
            title="control_flow.lamo"
          />
          <Alert type="info" title="Truthiness">
            In a boolean context: <code className="font-mono">0</code>,{' '}
            <code className="font-mono">0.0</code> and <code className="font-mono">""</code> are
            falsy; non-empty strings and non-zero values are truthy. Passing <code className="font-mono">void</code> to a
            condition is a compile-time error (SPEC §6.3).
          </Alert>
        </Section>

        <Section id="functions" title="Functions">
          <p>
            Functions are declared with <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">fn</code>{' '}
            and are hoisted within their file, allowing calls before definition. Parameter and
            return annotations are optional but strictly checked when provided.
          </p>
          <CodeBlock
            code={`fn add(a: int, b: int) -> int {\n    return a + b;\n}\n\nfn greet(name) {          // inferred parameter type\n    print("hi " + name);\n}\n\nfn main() {\n    print(add(2, 40));    // 42\n    greet("Lamo");\n}`}
            title="functions.lamo"
          />
          <p>
            Programs execute from{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">fn main()</code>.
            Standard builtins include <code className="font-mono text-sm">print</code>,{' '}
            <code className="font-mono text-sm">input</code>,{' '}
            <code className="font-mono text-sm">input_int</code>,{' '}
            <code className="font-mono text-sm">abs</code>,{' '}
            <code className="font-mono text-sm">exit</code>,{' '}
            <code className="font-mono text-sm">len</code>,{' '}
            <code className="font-mono text-sm">push</code>,{' '}
            <code className="font-mono text-sm">pop</code>.
          </p>
        </Section>

        <Section id="structs" title="Structs & Methods">
          <p>
            Structs declare typed fields. Methods live in{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">impl</code>{' '}
            blocks with an implicit <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">self</code> parameter.
          </p>
          <CodeBlock
            code={`struct Point {\n    x: int,\n    y: int,\n}\n\nimpl Point {\n    fn distance2(other: Point) -> int {\n        let dx = self.x - other.x;\n        let dy = self.y - other.y;\n        return dx * dx + dy * dy;\n    }\n}\n\nfn main() {\n    let a = Point { x: 0, y: 0 };\n    let b = Point { x: 3, y: 4 };\n    print(a.distance2(b));   // 25\n}`}
            title="structs.lamo"
          />
        </Section>

        <Section id="enums-match" title="Enums & Match">
          <p>
            Enums support both plain C-style integer variants and{' '}
            <strong>tagged unions</strong> (2.6.0) whose variants carry payloads.{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">match</code>{' '}
            destructures them with nested patterns, literal patterns and{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">when</code> guards.
          </p>
          <CodeBlock
            code={`enum Direction { North, South, East, West }\n\nenum Option<T> {\n    Some(T),\n    None,\n}\n\nfn describe(o: Option<int>) -> string {\n    match o {\n        Some(x) when x > 100 => "big: " + x,\n        Some(x) => "got " + x,\n        None => "nothing here",\n    }\n}\n\nfn main() {\n    let o: Option<int> = Some(42);\n    print(describe(o));      // got 42\n\n    match 2 {\n        1 => print("one"),\n        2 => print("two"),   // literal patterns\n        _ => print("other"),\n    }\n}`}
            title="match.lamo"
          />
          <p>
            Match arms are checked for exhaustiveness. Guarded arms do not count toward exhaustiveness, while a{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">_</code> wildcard covers remaining possibilities.
            Match can also be used as an expression (2.8.0).
          </p>
        </Section>

        <Section id="generics" title="Generics">
          <p>
            Generic structs, functions and impl blocks (2.5.0) feature call-site inference. The
            supported constraint catalogue is{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">Any | Eq | Ord | Num | Hash | Show</code>.
          </p>
          <CodeBlock
            code={`fn id<T>(x: T) -> T {\n    return x;\n}\n\nstruct Pair<A, B> {\n    first: A,\n    second: B,\n}\n\nimpl<T> Stack<T> { ... }   // generic impl blocks\n\nfn max2<T: Ord>(a: T, b: T) -> T {\n    if (a > b) { return a; }\n    return b;\n}\n\nfn main() {\n    print(id(42));                    // T inferred = int\n    let p = Pair<string, int> { first: "age", second: 30 };\n    print(max2(3, 7));                // 7\n}`}
            title="generics.lamo"
          />
        </Section>

        <Section id="traits" title="Traits">
          <p>
            Traits (2.9.0) declare compile-time contracts;{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">impl Trait for Type</code>{' '}
            satisfies them. Trait names double as generic constraints, and 2.10.0 dispatches
            trait methods on type parameters through generated{' '}
            <em>dictionaries</em> — plain C structs of function pointers, with zero vtable overhead.
          </p>
          <CodeBlock
            code={`trait Area {\n    fn area() -> float;\n}\n\nstruct Circle { r: float }\n\nimpl Area for Circle {\n    fn area() -> float {\n        return 3.14159 * self.r * self.r;\n    }\n}\n\nfn describe<T: Area>(shape: T) {\n    // dictionary dispatch: shape.area() invokes via LamoDict_Area\n    print(shape.area());\n}\n\nfn main() {\n    let c = Circle { r: 2.0 };\n    describe(c);   // 12.56636\n}`}
            title="traits.lamo"
          />
          <Alert type="info" title="Coherence">
            Trait implementations are validated for coherence and completeness: an impl that
            omits a required method or alters signatures causes a compilation error (SPEC §3.7).
          </Alert>
        </Section>

        <Section id="memory-style" title="Memory & Style">
          <p>
            The runtime incorporates a compact mark-sweep garbage collector — no manual{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">free</code>.
            Strings live in an arena and are immutable. In the{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">lamo eval</code>{' '}
            interpreter, arrays and structs are shared, reference-counted objects with byte-for-byte parity to compiled C.
          </p>
          <p>
            Canonical code style is formatted by{' '}
            <code className="rounded-md bg-purple-50 dark:bg-purple-950/60 px-1.5 py-0.5 font-mono text-sm text-purple-700 dark:text-purple-300 ring-1 ring-inset ring-purple-500/20">lamo fmt</code>:
            4-space indents, one statement per line, explicit semicolons, with comments accurately preserved and re-attached.
          </p>
          <Alert type="warning" title="Formatting Contract">
            <code className="font-mono">lamo fmt</code> is AST-based: it will never rewrite a
            file it cannot fully parse — unparseable files fall back to whitespace-only normalization.
          </Alert>
        </Section>
      </div>
    </DocsLayout>
  )
}
