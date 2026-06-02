import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

export default defineConfig({
  plugins: [vue()],
  server: {
    port: 5173,
    host: '0.0.0.0',
    watch: {
      usePolling: true,
      interval: 100
    },
    proxy: {
      // /api/* requests proxy to the standalone novaworld server
      // (apps/novaworld_server/opennova-novaworld) which binds 8080 by default.
      // Override with VITE_API_TARGET if you're hitting a different host.
      '/api': {
        target: process.env.VITE_API_TARGET || 'http://127.0.0.1:8080',
        changeOrigin: true
      }
    }
  }
});
