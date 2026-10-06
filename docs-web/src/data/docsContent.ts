import data from './generated/docs.json';
import type { DocCategory, SearchEntry } from '../types/docs';

// Public inputs live in docs-web/content/ and examples/. Run npm run docs:generate.
export const DOC_CATEGORIES: DocCategory[] = data.categories;
export const SEARCH_INDEX: SearchEntry[] = data.search;
export const DOC_METADATA = { version: data.version, reviewedRevision: data.reviewedRevision, sourceDigest: data.sourceDigest, platforms: data.platforms, limitations: data.limitations };
