import React, { useState, useEffect, useRef } from 'react';
import { Check, Copy, AlertCircle, Info, Lightbulb, AlertTriangle, ChevronRight, ChevronLeft } from 'lucide-react';
import { DocArticle } from '../types/docs';


interface DocContentProps {
  article: DocArticle;
  prevArticle?: DocArticle | null;
  nextArticle?: DocArticle | null;
  onNavigate: (articleId: string) => void;
}

export const DocContent: React.FC<DocContentProps> = ({
  article,
  prevArticle,
  nextArticle,
  onNavigate
}) => {
  const [selection, setSelection] = useState({articleId: article.id, index: 0});
  const activeTabIdx = selection.articleId === article.id && selection.index < (article.codeTabs?.length || 0) ? selection.index : 0;
  const [copyStatus, setCopyStatus] = useState('Copy');
  const copyAttempt = useRef(0);
  const copyTimer = useRef<ReturnType<typeof setTimeout>>();
  useEffect(() => { setCopyStatus('Copy'); return () => { copyAttempt.current++; clearTimeout(copyTimer.current); }; }, [article.id]);
  const handleCopyCode = async (code: string) => {
    const attempt = ++copyAttempt.current;
    clearTimeout(copyTimer.current);
    try {
      await navigator.clipboard.writeText(code);
      if (attempt !== copyAttempt.current) return;
      setCopyStatus('Copied!');
    } catch {
      if (attempt !== copyAttempt.current) return;
      setCopyStatus('Copy failed');
    }
    copyTimer.current = setTimeout(() => setCopyStatus('Copy'), 2000);
  };

  const getCalloutIcon = (type: string) => {
    switch (type) {
      case 'tip':
        return <Lightbulb size={20} />;
      case 'warning':
        return <AlertTriangle size={20} />;
      case 'important':
        return <AlertCircle size={20} />;
      default:
        return <Info size={20} />;
    }
  };

  return (
    <div>
      {/* Breadcrumbs */}
      <div className="doc-breadcrumbs">
        <span>Docs</span>
        <span>/</span>
        <span>{article.category}</span>
        <span>/</span>
        <span style={{ color: 'var(--text-primary)' }}>{article.title}</span>
      </div>

      {/* Body Content */}
      <div
        className="prose"
        dangerouslySetInnerHTML={{
          __html: article.html
        }}
      />

      {/* Code Snippet Tabs */}
      {article.codeTabs && article.codeTabs.length > 0 && (
        <div className="code-block-wrapper">
          <div className="code-header">
            <div className="code-tabs" role="tablist" aria-label="Code examples">
              {article.codeTabs.map((tab, idx) => (
                <button
                  key={tab.label}
                  role="tab"
                  aria-selected={activeTabIdx === idx}
                  aria-controls={`${article.id}-code`}
                  tabIndex={activeTabIdx === idx ? 0 : -1}
                  onKeyDown={event => {
                    const count = article.codeTabs!.length;
                    const next = event.key === 'ArrowRight' ? (idx + 1) % count : event.key === 'ArrowLeft' ? (idx + count - 1) % count : event.key === 'Home' ? 0 : event.key === 'End' ? count - 1 : -1;
                    if (next < 0) return;
                    event.preventDefault(); setSelection({articleId: article.id, index: next});
                    copyAttempt.current++; setCopyStatus('Copy');
                    event.currentTarget.parentElement?.querySelectorAll('button')[next].focus();
                  }}
                  className={`code-tab-btn ${activeTabIdx === idx ? 'active' : ''}`}
                  onClick={() => { setSelection({articleId: article.id, index: idx}); setCopyStatus("Copy"); copyAttempt.current++; }}
                >
                  {tab.label}
                </button>
              ))}
            </div>
            <button
              className="copy-btn"
              aria-label="Copy code"
              onClick={() => handleCopyCode(article.codeTabs![activeTabIdx].code)}
            >
              {copyStatus === 'Copied!' ? <Check size={14} color="var(--accent-emerald)" /> : <Copy size={14} />}
              <span role="status">{copyStatus}</span>
            </button>
          </div>
          {article.codeTabs[activeTabIdx].example && <div className="example-metadata">
            <p>{article.codeTabs[activeTabIdx].example!.mode === 'fragment' ? 'Illustrative fragment' : article.codeTabs[activeTabIdx].example!.mode === 'compile' ? 'Compiled integration example' : 'Executable example'} · {article.codeTabs[activeTabIdx].example!.id} · Setup group: {article.codeTabs[activeTabIdx].example!.setupGroup} · Checks: {article.codeTabs[activeTabIdx].example!.platforms.join(', ')}</p>
            {article.codeTabs[activeTabIdx].example!.setup.length > 0 && <p>Run after: {article.codeTabs[activeTabIdx].example!.setup.join(', ')}</p>}
            {article.codeTabs[activeTabIdx].example!.limitations && <p>{article.codeTabs[activeTabIdx].example!.limitations}</p>}
            {article.codeTabs[activeTabIdx].example!.output != null && <details><summary>Recorded engine output · {article.codeTabs[activeTabIdx].example!.revision.slice(0,12)}</summary><pre>{JSON.stringify(article.codeTabs[activeTabIdx].example!.output,null,2)}</pre></details>}
          </div>}
          <pre className="code-content" id={`${article.id}-code`} role="tabpanel">
            <code>{article.codeTabs[activeTabIdx].code}</code>
          </pre>
        </div>
      )}

      {/* Callouts */}
      {article.callouts?.map((c, idx) => (
        <div key={idx} className={`callout ${c.type}`}>
          <div className="callout-icon">{getCalloutIcon(c.type)}</div>
          <div className="callout-content">
            <div className="callout-title">{c.title}</div>
            <div>{c.message}</div>
          </div>
        </div>
      ))}

      {/* Parameters Table */}
      {article.parameters && article.parameters.length > 0 && (
        <div>
          <h2 style={{ fontSize: '1.25rem', fontFamily: 'var(--font-display)', margin: '28px 0 12px' }}>
            Configuration Parameters
          </h2>
          <div className="data-table-container">
            <table className="data-table">
              <thead>
                <tr>
                  <th>Parameter</th>
                  <th>Type</th>
                  <th>Default</th>
                  <th>Description</th>
                </tr>
              </thead>
              <tbody>
                {article.parameters.map((p) => (
                  <tr key={p.name}>
                    <td>
                      <code>{p.name}</code>
                    </td>
                    <td style={{ color: 'var(--accent-cyan)', fontFamily: 'var(--font-mono)' }}>{p.type}</td>
                    <td>{p.defaultVal ? <code>{p.defaultVal}</code> : <span style={{ color: 'var(--text-muted)' }}>required</span>}</td>
                    <td>{p.description}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      {/* Pagination Footer */}
      <div className="doc-pagination">
        {prevArticle ? (
          <a
            className="pagination-card" href={`#${prevArticle.id}`}
            onClick={() => onNavigate(prevArticle.id)}
            style={{ cursor: 'pointer' }}
          >
            <span className="pagination-label">
              <ChevronLeft size={12} style={{ display: 'inline', verticalAlign: 'middle' }} /> Previous
            </span>
            <span className="pagination-title">{prevArticle.title}</span>
          </a>
        ) : (
          <div />
        )}

        {nextArticle ? (
          <a
            className="pagination-card" href={`#${nextArticle.id}`}
            onClick={() => onNavigate(nextArticle.id)}
            style={{ cursor: 'pointer', textAlign: 'right' }}
          >
            <span className="pagination-label">
              Next <ChevronRight size={12} style={{ display: 'inline', verticalAlign: 'middle' }} />
            </span>
            <span className="pagination-title">{nextArticle.title}</span>
          </a>
        ) : (
          <div />
        )}
      </div>
    </div>
  );
};
