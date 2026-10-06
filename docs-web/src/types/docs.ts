export interface CodeTab {
  label: string;
  language: string;
  code: string;
  example?: {id: string; mode: string; setupGroup: string; setup: string[]; platforms: string[]; limitations: string; revision: string; output: unknown};
}

export interface ParameterDoc {
  name: string;
  type: string;
  defaultVal?: string;
  description: string;
}

export interface CalloutDoc {
  type: 'tip' | 'warning' | 'important' | 'note';
  title: string;
  message: string;
}

export interface DocArticle {
  id: string;
  title: string;
  category: string;
  lead: string;
  content: string; // Canonical Markdown
  html: string; // Generated HTML with validated local links
  callouts?: CalloutDoc[];
  codeTabs?: CodeTab[];
  parameters?: ParameterDoc[];
  returnsDoc?: string;
  relatedIds?: string[];
}

export interface DocCategory {
  id: string;
  title: string;
  articles: DocArticle[];
}

export interface SearchEntry {
  id: string;
  articleId: string;
  title: string;
  category: string;
  preview: string;
  content?: string;
  keywords?: string[];
}

// Graph Visualizer Types
export interface VisualNode {
  id: string;
  label: string;
  properties: Record<string, any>;
  x?: number;
  y?: number;
  vx?: number;
  vy?: number;
}

export interface VisualEdge {
  from: string;
  type: string;
  to: string;
  properties?: Record<string, any>;
}

export interface VisualGraph {
  nodes: VisualNode[];
  edges: VisualEdge[];
}
