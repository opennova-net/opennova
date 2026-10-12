import { createRouter, createWebHistory } from 'vue-router';
import LandingPage from '../pages/LandingPage.vue';
import LoginPage from '../pages/LoginPage.vue';
import RegisterPage from '../pages/RegisterPage.vue';
import LobbyPage from '../pages/LobbyPage.vue';
import MyServersPage from '../pages/MyServersPage.vue';
import AdminLayout from '../pages/admin/AdminLayout.vue';
import UsersSection from '../pages/admin/UsersSection.vue';
import ServerStatusSection from '../pages/admin/ServerStatusSection.vue';
import ConnectionsSection from '../pages/admin/ConnectionsSection.vue';
import { requireUser } from './guards';

// A page only a logged-in visitor may open sets `meta: { requiresUser: true }`
// (guards.ts). /admin does not: its AdminGate also takes the Bearer token.
const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', component: LandingPage },
    { path: '/login', component: LoginPage },
    { path: '/register', component: RegisterPage },
    { path: '/lobby', component: LobbyPage },
    { path: '/me/servers', component: MyServersPage, meta: { requiresUser: true } },
    {
      path: '/admin',
      component: AdminLayout,
      children: [
        { path: '', redirect: '/admin/users' },
        { path: 'users', component: UsersSection },
        { path: 'server', component: ServerStatusSection },
        { path: 'connections', component: ConnectionsSection }
      ]
    }
  ],
  scrollBehavior() {
    return { top: 0 };
  }
});

router.beforeEach(requireUser);

export default router;
