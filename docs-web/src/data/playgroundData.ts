import data from './generated/docs.json';

export interface PlaygroundPreset {
  id: string;
  name: string;
  description: string;
  query: string;
  recorded: {revision: string; outputText: string};
}

export const PLAYGROUND_PRESETS: PlaygroundPreset[] = data.presets;
