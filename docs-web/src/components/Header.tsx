import React from 'react';
import { Search, Sun, Moon, Play, Menu, X } from 'lucide-react';

interface HeaderProps {
  currentView: 'docs' | 'playground';
  onSelectView: (view: 'docs' | 'playground') => void;
  onOpenSearch: () => void;
  isDarkTheme: boolean;
  onToggleTheme: () => void;
  isMobileOpen: boolean;
  onToggleMobile: () => void;
}

export const Header: React.FC<HeaderProps> = ({
  currentView,
  onSelectView,
  onOpenSearch,
  isDarkTheme,
  onToggleTheme,
  isMobileOpen,
  onToggleMobile
}) => {
  return (
    <header className="site-header">
      <div className="header-left">
        <button className="icon-btn mobile-menu-toggle" aria-label={isMobileOpen ? "Close navigation" : "Open navigation"} aria-expanded={isMobileOpen} aria-controls="docs-navigation" onClick={onToggleMobile}>
          {isMobileOpen ? <X size={20} /> : <Menu size={20} />}
        </button>

        <a className="logo-badge" href="#overview">
          <svg className="logo-icon-svg" viewBox="0 0 100 100">
            <circle cx="50" cy="50" r="46" fill="#6366f1" />
            <circle cx="30" cy="40" r="12" fill="#ffffff" />
            <circle cx="70" cy="40" r="12" fill="#ffffff" />
            <circle cx="50" cy="75" r="14" fill="#38bdf8" />
            <path d="M30 40 L50 75 L70 40" stroke="#ffffff" strokeWidth="6" strokeLinecap="round" />
          </svg>
          <span className="logo-title">NovaGraph</span>
        </a>
      </div>

      <div className="header-center">
        <button className="search-trigger-btn" onClick={onOpenSearch}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            <Search size={16} />
            <span>Search docs, NGQL, APIs...</span>
          </div>
          <div className="search-shortcut-badge">
            <span>⌘</span>
            <span>K</span>
          </div>
        </button>
      </div>

      <div className="header-right">
        <button
          className={`nav-pill-btn ${currentView === 'docs' ? 'secondary' : 'secondary'}`}
          onClick={() => onSelectView('docs')}
        >
          Docs
        </button>

        <button
          className={`nav-pill-btn ${currentView === 'playground' ? 'primary' : 'secondary'}`}
          onClick={() => onSelectView('playground')}
        >
          <Play size={13} fill={currentView === 'playground' ? '#ffffff' : 'currentColor'} />
          <span>Playground</span>
        </button>


        <button
          className="icon-btn"
          onClick={onToggleTheme}
          title={isDarkTheme ? 'Switch to Light Mode' : 'Switch to Dark Mode'}
        >
          {isDarkTheme ? <Sun size={18} /> : <Moon size={18} />}
        </button>
      </div>
    </header>
  );
};
