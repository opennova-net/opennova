import { createApp } from 'vue';
import App from './App.vue';
import router from './router';
import { loadSession } from './composables/useSession';
import './assets/tailwind.css';

// Who is logged in, read once from /api/me while the app mounts; the NavBar,
// the admin gate and the router guard wait on this one request.
void loadSession();

const app = createApp(App);
app.use(router);
app.mount('#app');
