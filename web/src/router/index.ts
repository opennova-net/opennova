import { createRouter, createWebHistory } from 'vue-router';
import LandingPage from '../pages/LandingPage.vue';
import LoginPage from '../pages/LoginPage.vue';
import RegisterPage from '../pages/RegisterPage.vue';
import ForgotPasswordPage from '../pages/ForgotPasswordPage.vue';
import ResetPasswordPage from '../pages/ResetPasswordPage.vue';
import LobbyPage from '../pages/LobbyPage.vue';
import AccountLayout from '../pages/account/AccountLayout.vue';
import ServersPage from '../pages/account/ServersPage.vue';
import NewServerPage from '../pages/account/NewServerPage.vue';
import AccountPage from '../pages/account/AccountPage.vue';
import SupportTierPage from '../pages/account/SupportTierPage.vue';
import SupportPage from '../pages/SupportPage.vue';
import NotFoundPage from '../pages/NotFoundPage.vue';
import TermsPage from '../pages/legal/TermsPage.vue';
import PrivacyPage from '../pages/legal/PrivacyPage.vue';
import RefundsPage from '../pages/legal/RefundsPage.vue';
import ServerDetailLayout from '../pages/servers/ServerDetailLayout.vue';
import ServerOverviewTab from '../pages/servers/ServerOverviewTab.vue';
import ServerConsoleTab from '../pages/servers/ServerConsoleTab.vue';
import ServerStatsTab from '../pages/servers/ServerStatsTab.vue';
import ServerSettingsTab from '../pages/servers/ServerSettingsTab.vue';
import ServerMachineTab from '../pages/servers/ServerMachineTab.vue';
import AdminLayout from '../pages/admin/AdminLayout.vue';
import OverviewSection from '../pages/admin/OverviewSection.vue';
import UsersSection from '../pages/admin/UsersSection.vue';
import UserDetailSection from '../pages/admin/UserDetailSection.vue';
import FleetSection from '../pages/admin/FleetSection.vue';
import UsageSection from '../pages/admin/UsageSection.vue';
import BillingSection from '../pages/admin/BillingSection.vue';
import ServerStatusSection from '../pages/admin/ServerStatusSection.vue';
import ConnectionsSection from '../pages/admin/ConnectionsSection.vue';
import CdKeysSection from '../pages/admin/CdKeysSection.vue';
import { requireUser } from './guards';

// A page only a logged-in visitor may open sets `meta: { requiresUser: true }`
// (guards.ts). /admin does not: its AdminGate also takes the Bearer token.
const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', component: LandingPage },
    { path: '/login', component: LoginPage, meta: { title: 'Log in' } },
    { path: '/register', component: RegisterPage, meta: { title: 'Register' } },
    { path: '/forgot', component: ForgotPasswordPage, meta: { title: 'Forgot password' } },
    // The link a password reset mail carries: /reset?token=<64 hex digits>.
    { path: '/reset', component: ResetPasswordPage, meta: { title: 'Reset password' } },
    { path: '/lobby', component: LobbyPage, meta: { title: 'Lobbies' } },
    { path: '/support', component: SupportPage, meta: { title: 'Support tiers' } },
    { path: '/terms', component: TermsPage, meta: { title: 'Terms of service' } },
    { path: '/privacy', component: PrivacyPage, meta: { title: 'Privacy notice' } },
    { path: '/refunds', component: RefundsPage, meta: { title: 'Refund policy' } },
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
            { path: 'stats', component: ServerStatsTab },
            { path: 'console', component: ServerConsoleTab },
            { path: 'settings', component: ServerSettingsTab },
          ],
        },
        // Stripe's Checkout and billing portal return here
        // (?checkout=success&session_id=..., ?checkout=cancel).
        { path: 'support', component: SupportTierPage },
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
        { path: '', redirect: '/admin/overview' },
        { path: 'overview', component: OverviewSection },
        { path: 'users', component: UsersSection },
        { path: 'users/:id(\\d+)', component: UserDetailSection },
        { path: 'fleet', component: FleetSection },
        {
          path: 'fleet/:id(\\d+)',
          component: ServerDetailLayout,
          props: { scope: 'admin' },
          children: [
            { path: '', component: ServerOverviewTab },
            { path: 'stats', component: ServerStatsTab },
            { path: 'console', component: ServerConsoleTab },
            { path: 'game-settings', component: ServerSettingsTab },
            { path: 'machine', component: ServerMachineTab },
          ],
        },
        { path: 'usage', component: UsageSection },
        { path: 'billing', component: BillingSection },
        { path: 'service', component: ServerStatusSection },
        { path: 'sessions', component: ConnectionsSection },
        { path: 'cd-keys', component: CdKeysSection },
        // The sections' names before the admin pages were rebuilt: old links
        // land on their new pages.
        { path: 'hosted', redirect: '/admin/fleet' },
        { path: 'server', redirect: '/admin/service' },
        { path: 'connections', redirect: '/admin/sessions' },
      ],
    },
    // Anything else.
    { path: '/:rest(.*)*', component: NotFoundPage, meta: { title: 'Page not found' } },
  ],
  // Back and Forward return to where the page was; a #section link scrolls to
  // it below the fixed header; a server page's tab change keeps the scroll (the
  // header and the tabs stay put); any other navigation starts at the top.
  scrollBehavior(to, from, savedPosition) {
    if (savedPosition) return savedPosition;
    if (to.hash) return { el: to.hash, top: 72 };
    const onServerPage = to.matched.some((record) => record.components?.default === ServerDetailLayout);
    if (onServerPage && to.params.id === from.params.id) return false;
    return { top: 0 };
  },
});

router.beforeEach(requireUser);

// Each public page names itself in the tab; the rest keep the site's title.
const SITE_TITLE = document.title;
router.afterEach((to) => {
  const title = to.meta.title as string | undefined;
  document.title = title ? `${title} | OpenNova` : SITE_TITLE;
});

export default router;
