import React, { useState, useEffect, useRef } from 'react';
import { Search, X, ArrowRight, CornerDownLeft } from 'lucide-react';
import { SEARCH_INDEX } from '../data/docsContent';
import { SearchEntry } from '../types/docs';

interface SearchModalProps {
  isOpen: boolean;
  onClose: () => void;
  onSelectResult: (articleId: string) => void;
}

export const SearchModal: React.FC<SearchModalProps> = ({
  isOpen,
  onClose,
  onSelectResult
}) => {
  const [query, setQuery] = useState('');
  const [selectedIndex, setSelectedIndex] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);

  const dialogRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!isOpen) return;
    const previous = document.activeElement as HTMLElement | null;
    setQuery(''); setSelectedIndex(0);
    inputRef.current?.focus();
    return () => previous?.focus();
  }, [isOpen]);

  const filteredResults: SearchEntry[] = query.trim() === ''
    ? SEARCH_INDEX.slice(0, 6)
    : SEARCH_INDEX.filter((item) => {
        const q = query.toLowerCase();
        return (
          item.title.toLowerCase().includes(q) ||
          item.category.toLowerCase().includes(q) ||
          item.preview.toLowerCase().includes(q) ||
          item.content?.toLowerCase().includes(q) ||
          item.keywords?.some((k) => k.toLowerCase().includes(q))
        );
      });

  const handleKeyDown = (e: React.KeyboardEvent) => {
    // Dialog-wide focus/Escape handling must not override native button activation.
    if (e.key !== 'Tab' && e.key !== 'Escape' && e.target !== inputRef.current) return;
    if (e.key === 'Tab') {
      const controls = dialogRef.current?.querySelectorAll<HTMLElement>('input, button');
      if (controls?.length && e.shiftKey && document.activeElement === controls[0]) { e.preventDefault(); controls[controls.length - 1].focus(); }
      else if (controls?.length && !e.shiftKey && document.activeElement === controls[controls.length - 1]) { e.preventDefault(); controls[0].focus(); }
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      setSelectedIndex((prev) => filteredResults.length ? (prev + 1) % filteredResults.length : 0);
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      setSelectedIndex((prev) => filteredResults.length ? (prev - 1 + filteredResults.length) % filteredResults.length : 0);
    } else if (e.key === 'Enter') {
      e.preventDefault();
      if (filteredResults[selectedIndex]) {
        onSelectResult(filteredResults[selectedIndex].articleId);
        onClose();
      }
    } else if (e.key === 'Escape') {
      onClose();
    }
  };

  if (!isOpen) return null;

  return (
    <div className="search-modal-backdrop" onClick={onClose}>
      <div ref={dialogRef} className="search-modal-box" role="dialog" aria-modal="true" aria-label="Search documentation" onKeyDown={handleKeyDown} onClick={(e) => e.stopPropagation()}>
        <div className="search-input-wrapper">
          <Search size={20} color="var(--text-muted)" />
          <input
            ref={inputRef}
            role="combobox" aria-label="Search documentation" aria-autocomplete="list" aria-expanded="true" aria-controls="search-results" aria-activedescendant={filteredResults[selectedIndex]?.id}
            className="search-input"
            placeholder="Search documentation, NGQL statements, methods..."
            value={query}
            onChange={(e) => {
              setQuery(e.target.value);
              setSelectedIndex(0);
            }}
          />
          <button className="icon-btn" aria-label="Close search" onClick={onClose}>
            <X size={18} />
          </button>
        </div>

        <div className="search-results-list" role="listbox" id="search-results" aria-label="Search results">
          {filteredResults.length === 0 ? (
            <div style={{ padding: '32px 20px', textAlign: 'center', color: 'var(--text-muted)' }}>
              No matching documentation found for "{query}".
            </div>
          ) : (
            filteredResults.map((result, idx) => (
              <div
                key={result.id}
                id={result.id} role="option" aria-selected={selectedIndex === idx}
                className={`search-item ${selectedIndex === idx ? 'selected' : ''}`}
                onClick={() => {
                  onSelectResult(result.articleId);
                  onClose();
                }}
              >
                <div className="search-item-title">
                  <span>{result.title}</span>
                  <span className="search-item-category">{result.category}</span>
                  <ArrowRight size={14} style={{ marginLeft: 'auto', opacity: 0.5 }} />
                </div>
                <div className="search-item-preview">{result.preview}</div>
              </div>
            ))
          )}
        </div>

        <div className="search-modal-footer">
          <div style={{ display: 'flex', gap: 12 }}>
            <span><kbd>↑</kbd> <kbd>↓</kbd> to navigate</span>
            <span><kbd><CornerDownLeft size={10} style={{ display: 'inline' }} /></kbd> to select</span>
            <span><kbd>esc</kbd> to close</span>
          </div>
          <span>{filteredResults.length} results</span>
        </div>
      </div>
    </div>
  );
};
