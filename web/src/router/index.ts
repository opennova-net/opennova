import { createRouter, createWebHistory } from 'vue-router';
import LandingPage from '../pages/LandingPage.vue';
import LoginPage from '../pages/LoginPage.vue';
import RegisterPage from '../pages/RegisterPage.vue';
import LobbyPage from '../pages/LobbyPage.vue';
import AccountLayout from '../pages/account/AccountLayout.vue';
import ServersPage from '../pages/account/ServersPage.vue';
import NewServerPage from '../pages/account/NewServerPage.vue';
import AccountPage from '../pages/account/AccountPage.vue';
import ServerDetailLayout from '../pages/servers/ServerDetailLayout.vue';
import ServerOverviewTab from '../pages/servers/ServerOverviewTab.vue';
import ServerConsoleTab from '../pages/servers/ServerConsoleTab.vue';
import ServerSettingsTab from '../pages/servers/ServerSettingsTab.vue';
import AdminLayout from '../pages/admin/AdminLayout.vue';
import UsersSection from '../pages/admin/UsersSection.vue';
import ServerStatusSection from '../pages/admin/ServerStatusSection.vue';
import ConnectionsSection from '../pages/admin/ConnectionsSection.vue';
import HostedSection from '../pages/admin/HostedSection.vue';
import CdKeysSection from '../pages/admin/CdKeysSection.vue';
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
    // The account area.
    {
      path: '/me',
      component: AccountLayout,
      meta: { requiresUser: true },
      children: [
        { path: '', redirect: '/me/servers' },
        { path: 'servers', component: ServersPage },
        { path: 'servers/new', component: NewServerPage },
        {
          path: 'servers/:id(\\d+)',
          component: ServerDetailLayout,
          props: { scope: 'owner' },
          children: [
            { path: '', component: ServerOverviewTab },
            { path: 'console', component: ServerConsoleTab },
            { path: 'settings', component: ServerSettingsTab },
          ],
        },
        { path: 'account', component: AccountPage },
        // The hosted server's page before an account could have several:
        // old links land on the list.
        { path: 'server', redirect: '/me/servers' },
      ],
    },
    {
      path: '/admin',
      component: AdminLayout,
      children: [
        { path: '', redirect: '/admin/users' },
        { path: 'users', component: UsersSection },
        { path: 'server', component: ServerStatusSection },
        { path: 'connections', component: ConnectionsSection },
        { path: 'hosted', component: HostedSection },
        { path: 'cd-keys', component: CdKeysSection },
      ],
    },
  ],
  // A server page's tab change keeps the scroll (the header and the tabs stay
  // put); any other navigation starts at the top.
  scrollBehavior(to, from) {
    const onServerPage = to.matched.some((record) => record.components?.default === ServerDetailLayout);
    if (onServerPage && to.params.id === from.params.id) return false;
    return { top: 0 };
  },
});

router.beforeEach(requireUser);

export default router;
