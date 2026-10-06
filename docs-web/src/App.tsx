import React, { useState, useEffect } from 'react';
import { DOC_CATEGORIES } from './data/docsContent';
import { Header } from './components/Header';
import { Sidebar } from './components/Sidebar';
import { DocContent } from './components/DocContent';
import { Playground } from './components/Playground';
import { SearchModal } from './components/SearchModal';
import { DocArticle } from './types/docs';

export const App: React.FC = () => {
  const [currentView, setCurrentView] = useState<'docs' | 'playground'>('docs');
  const [currentArticleId, setCurrentArticleId] = useState<string>('overview');
  const [isSearchOpen, setIsSearchOpen] = useState(false);
  const [isMobileOpen, setIsMobileOpen] = useState(false);
  const [isDarkTheme, setIsDarkTheme] = useState(true);

  // Keep canonical Markdown links, reloads and browser history on the same route.
  useEffect(() => {
    const followHash = () => {
      const id = window.location.hash.slice(1).split('/')[0] || 'overview';
      if (id === 'playground') { setCurrentView('playground'); setIsMobileOpen(false); return; }
      setCurrentArticleId(id);
      setCurrentView('docs');
      setIsMobileOpen(false);
    };
    followHash();
    window.addEventListener('hashchange', followHash);
    return () => window.removeEventListener('hashchange', followHash);
  }, []);

  useEffect(() => {
    const scroll = () => {
      const anchor = window.location.hash.slice(1);
      if (anchor.includes('/')) { try { document.getElementById(decodeURIComponent(anchor))?.scrollIntoView(); } catch { /* Malformed external URL; show the route fallback. */ } }
      else window.scrollTo({ top: 0 });
    };
    const frame = requestAnimationFrame(scroll);
    window.addEventListener('hashchange', scroll);
    return () => { cancelAnimationFrame(frame); window.removeEventListener('hashchange', scroll); };
  }, [currentArticleId, currentView]);

  // Initialize theme
  useEffect(() => {
    const savedTheme = localStorage.getItem('novagraph-docs-theme');
    if (savedTheme === 'light') {
      setIsDarkTheme(false);
      document.documentElement.setAttribute('data-theme', 'light');
    } else {
      setIsDarkTheme(true);
      document.documentElement.setAttribute('data-theme', 'dark');
    }
  }, []);

  const toggleTheme = () => {
    const nextTheme = !isDarkTheme;
    setIsDarkTheme(nextTheme);
    const themeName = nextTheme ? 'dark' : 'light';
    document.documentElement.setAttribute('data-theme', themeName);
    localStorage.setItem('novagraph-docs-theme', themeName);
  };

  // Keyboard shortcut for search (Cmd+K / Ctrl+K)
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
        e.preventDefault();
        setIsSearchOpen(true);
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, []);

  // Flatten articles for pagination
  const allArticles: DocArticle[] = DOC_CATEGORIES.flatMap((c) => c.articles);
  const currentIndex = allArticles.findIndex((a) => a.id === currentArticleId);
  const currentArticle = allArticles[currentIndex];
  const prevArticle = currentIndex > 0 ? allArticles[currentIndex - 1] : null;
  const nextArticle = currentIndex < allArticles.length - 1 ? allArticles[currentIndex + 1] : null;

  const handleSelectArticle = (id: string) => {
    window.location.hash = id;
    setCurrentArticleId(id.split('/')[0]);
    setCurrentView('docs');
    window.scrollTo({ top: 0, behavior: 'smooth' });
  };

  return (
    <div className="app-container">
      <Header
        currentView={currentView}
        onSelectView={(view) => {
          window.location.hash = view === 'playground' ? 'playground' : currentArticleId;
          setCurrentView(view);
          window.scrollTo({ top: 0, behavior: 'smooth' });
        }}
        onOpenSearch={() => setIsSearchOpen(true)}
        isDarkTheme={isDarkTheme}
        onToggleTheme={toggleTheme}
        isMobileOpen={isMobileOpen}
        onToggleMobile={() => setIsMobileOpen(!isMobileOpen)}
      />

      <div className="main-layout">
        <Sidebar
          categories={DOC_CATEGORIES}
          currentArticleId={currentArticleId}
          onSelectArticle={handleSelectArticle}
          isOpenMobile={isMobileOpen}
          onCloseMobile={() => setIsMobileOpen(false)}
          onSelectPlayground={() => { window.location.hash = 'playground'; setCurrentView('playground'); }}
          currentView={currentView}
        />

        <main className="content-wrapper">
          {currentView === 'docs' ? (
            currentArticle ? <DocContent
              key={currentArticle.id}
              article={currentArticle}
              prevArticle={prevArticle}
              nextArticle={nextArticle}
              onNavigate={handleSelectArticle}
            /> : <section><h1>Article not found</h1><p>This documentation link does not exist.</p><a href="#overview">Open overview</a></section>
          ) : (
            <Playground />
          )}
        </main>
      </div>

      <SearchModal
        isOpen={isSearchOpen}
        onClose={() => setIsSearchOpen(false)}
        onSelectResult={handleSelectArticle}
      />
    </div>
  );
};

export default App;
