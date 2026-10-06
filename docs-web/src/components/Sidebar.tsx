import React from 'react';
import { BookOpen, Cpu, Terminal, Layers, Apple, BarChart3, Play } from 'lucide-react';
import { DocCategory } from '../types/docs';

interface SidebarProps {
  categories: DocCategory[];
  currentArticleId: string;
  onSelectArticle: (articleId: string) => void;
  isOpenMobile: boolean;
  onCloseMobile: () => void;
  onSelectPlayground: () => void;
  currentView: 'docs' | 'playground';
}

export const Sidebar: React.FC<SidebarProps> = ({
  categories,
  currentArticleId,
  onSelectArticle,
  isOpenMobile,
  onCloseMobile,
  onSelectPlayground,
  currentView
}) => {
  const getCategoryIcon = (categoryId: string) => {
    switch (categoryId) {
      case 'getting-started':
        return <BookOpen size={14} />;
      case 'architecture-internals':
        return <Cpu size={14} />;
      case 'ngql-reference':
        return <Terminal size={14} />;
      case 'swift-api-reference':
        return <Layers size={14} />;
      case 'apple-integration':
        return <Apple size={14} />;
      case 'benchmarks-tooling':
        return <BarChart3 size={14} />;
      default:
        return <BookOpen size={14} />;
    }
  };

  return (
    <aside id="docs-navigation" aria-label="Documentation navigation" className={`sidebar ${isOpenMobile ? 'open' : ''}`}>
      {/* Featured NGQL Playground Link */}
      <div style={{ marginBottom: 20 }}>
        <a
          href="#playground"
          className={`sidebar-link ${currentView === 'playground' ? 'active' : ''}`}
          onClick={() => {
            onSelectPlayground();
            onCloseMobile();
          }}
          style={{ background: currentView === 'playground' ? undefined : 'linear-gradient(135deg, rgba(99,102,241,0.12), rgba(56,189,248,0.12))', border: '1px solid rgba(99,102,241,0.25)' }}
        >
          <Play size={16} color="#6366f1" />
          <span style={{ fontWeight: 600 }}>NGQL Playground</span>
          <span className="sidebar-link-badge">EXAMPLES</span>
        </a>
      </div>

      {categories.map((category) => (
        <div key={category.id} className="sidebar-category">
          <div className="sidebar-category-title">
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              {getCategoryIcon(category.id)}
              <span>{category.title}</span>
            </div>
            <span>{category.articles.length}</span>
          </div>

          <div>
            {category.articles.map((article) => {
              const isActive = currentView === 'docs' && currentArticleId === article.id;
              return (
                <a
                  key={article.id}
                  href={`#${article.id}`}
                  aria-current={isActive ? "page" : undefined}
                  className={`sidebar-link ${isActive ? 'active' : ''}`}
                  onClick={() => {
                    onSelectArticle(article.id);
                    onCloseMobile();
                  }}
                >
                  <span>{article.title}</span>
                </a>
              );
            })}
          </div>
        </div>
      ))}
    </aside>
  );
};
