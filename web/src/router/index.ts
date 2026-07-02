import { createRouter, createWebHistory } from 'vue-router';
import LandingPage from '../pages/LandingPage.vue';
import RegisterPage from '../pages/RegisterPage.vue';
import LobbyPage from '../pages/LobbyPage.vue';
import ExpansionsPage from '../pages/ExpansionsPage.vue';
import ModToolsPage from '../pages/ModToolsPage.vue';
import ExpansionDetailPage from '../pages/expansions/ExpansionDetailPage.vue';
import AdminLayout from '../pages/admin/AdminLayout.vue';
import ReleasesSection from '../pages/admin/ReleasesSection.vue';
import UsersSection from '../pages/admin/UsersSection.vue';
import ServerStatusSection from '../pages/admin/ServerStatusSection.vue';
import ConnectionsSection from '../pages/admin/ConnectionsSection.vue';

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', component: LandingPage },
    { path: '/register', component: RegisterPage },
    { path: '/lobby', component: LobbyPage },
    { path: '/expansions', component: ExpansionsPage },
    { path: '/expansions/:slug', component: ExpansionDetailPage },
    { path: '/mod-tools', component: ModToolsPage },
    {
      path: '/admin',
      component: AdminLayout,
      children: [
        { path: '', redirect: '/admin/releases' },
        { path: 'releases', component: ReleasesSection },
        { path: 'expansions', redirect: '/admin/releases' },
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

export default router;
