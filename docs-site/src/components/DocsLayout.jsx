import Sidebar from './Sidebar.jsx'
import TableOfContents from './TableOfContents.jsx'

export default function DocsLayout({ children, showToc = false, tocItems }) {
  return (
    <div className="mx-auto flex max-w-7xl gap-10 px-4 py-8 sm:px-6 lg:px-8">
      {/* Left Navigation Sidebar */}
      <aside className="sticky top-20 hidden max-h-[calc(100vh-6rem)] w-60 shrink-0 overflow-y-auto pr-3 lg:block">
        <Sidebar />
      </aside>

      {/* Main Content */}
      <article className="min-w-0 flex-1 pb-16">{children}</article>

      {/* Right Table of Contents (for docs pages) */}
      {showToc && (
        <aside className="sticky top-24 hidden max-h-[calc(100vh-8rem)] w-56 shrink-0 overflow-y-auto pl-4 xl:block">
          <TableOfContents items={tocItems} />
        </aside>
      )}
    </div>
  )
}
