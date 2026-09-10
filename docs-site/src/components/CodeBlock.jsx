import { useState } from 'react'

export default function CodeBlock({ code, lang = 'lamo', title }) {
  const [copied, setCopied] = useState(false)

  const copy = async () => {
    try {
      await navigator.clipboard.writeText(code)
      setCopied(true)
      setTimeout(() => setCopied(false), 2000)
    } catch {
      /* clipboard unavailable */
    }
  }

  return (
    <div className="group relative my-4 overflow-hidden rounded-xl border border-purple-900/40 bg-[#0d091b] shadow-lg shadow-purple-950/20 transition-all dark:border-purple-500/20">
      {/* Terminal Header */}
      <div className="flex h-9 items-center justify-between border-b border-purple-900/30 bg-[#140e29]/70 px-4">
        {/* Window controls (traffic lights) */}
        <div className="flex items-center gap-1.5">
          <span className="h-2.5 w-2.5 rounded-full bg-rose-500/70" />
          <span className="h-2.5 w-2.5 rounded-full bg-amber-500/70" />
          <span className="h-2.5 w-2.5 rounded-full bg-emerald-500/70" />
          {title && (
            <span className="ml-2 font-mono text-[11px] font-medium text-gray-400">
              {title}
            </span>
          )}
        </div>

        {/* Right action: language badge & copy button */}
        <div className="flex items-center gap-2">
          {lang && !title && (
            <span className="rounded bg-purple-950/60 px-1.5 py-0.5 font-mono text-[11px] text-purple-300 ring-1 ring-inset ring-purple-500/20">
              {lang}
            </span>
          )}
          <button
            type="button"
            onClick={copy}
            aria-label="Copy code"
            className={`inline-flex items-center gap-1.5 rounded-md px-2 py-1 text-xs font-medium transition-all ${
              copied
                ? 'bg-emerald-500/20 text-emerald-300 ring-1 ring-emerald-500/40'
                : 'text-gray-400 hover:bg-purple-950/60 hover:text-purple-200'
            }`}
          >
            {copied ? (
              <>
                <svg className="h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
                  <path
                    fillRule="evenodd"
                    d="M16.707 5.293a1 1 0 010 1.414l-8 8a1 1 0 01-1.414 0l-4-4a1 1 0 011.414-1.414L8 12.586l7.293-7.293a1 1 0 011.414 0z"
                    clipRule="evenodd"
                  />
                </svg>
                <span>Copied!</span>
              </>
            ) : (
              <>
                <svg className="h-3.5 w-3.5" fill="none" viewBox="0 0 24 24" strokeWidth="1.75" stroke="currentColor">
                  <path
                    strokeLinecap="round"
                    strokeLinejoin="round"
                    d="M15.666 3.842A2.25 2.25 0 0013.5 2.25h-3c-1.03 0-1.9.693-2.166 1.592m7.332 0c.055.194.084.4.084.608v1.05a1.5 1.5 0 01-1.5 1.5h-4.5a1.5 1.5 0 01-1.5-1.5V4.45c0-.208.03-.414.084-.608m7.332 0a2.25 2.25 0 011.834 2.158V19.5a2.25 2.25 0 01-2.25 2.25H6.75a2.25 2.25 0 01-2.25-2.25V6a2.25 2.25 0 011.834-2.158"
                  />
                </svg>
                <span>Copy</span>
              </>
            )}
          </button>
        </div>
      </div>

      {/* Code body */}
      <pre className="overflow-x-auto p-4 text-[13px] leading-relaxed text-purple-100/90 font-mono">
        <code>{code}</code>
      </pre>
    </div>
  )
}
