// The web unit tests (npm test): vitest over vite.config.ts's plugins, the
// components in jsdom. Test files sit beside what they test (src/**/*.test.ts)
// and never reach the production build, which imports none of them.
import { defineConfig, mergeConfig } from 'vitest/config';
import viteConfig from './vite.config';

export default mergeConfig(
  viteConfig,
  defineConfig({
    test: {
      environment: 'jsdom',
      include: ['src/**/*.test.ts'],
    },
  }),
);
