import { createRouter, createWebHistory } from 'vue-router';
import LandingPage from '../pages/LandingPage.vue';
import LoginPage from '../pages/LoginPage.vue';
import RegisterPage from '../pages/RegisterPage.vue';
import ForgotPasswordPage from '../pages/ForgotPasswordPage.vue';
import ResetPasswordPage from '../pages/ResetPasswordPage.vue';
import LobbyPage from '../pages/LobbyPage.vue';
import LobbyServerPage from '../pages/LobbyServerPage.vue';
import AccountLayout from '../pages/account/AccountLayout.vue';
import ServersPage from '../pages/account/ServersPage.vue';
import NewServerPage from '../pages/account/NewServerPage.vue';
import AccountPage from '../pages/account/AccountPage.vue';
import SupportTierPage from '../pages/account/SupportTierPage.vue';
import AddressesPage from '../pages/account/AddressesPage.vue';
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
import ServerUsageTab from '../pages/servers/ServerUsageTab.vue';
import ServerLogsTab from '../pages/servers/ServerLogsTab.vue';
import AdminLayout from '../pages/admin/AdminLayout.vue';
import OverviewSection from '../pages/admin/OverviewSection.vue';
import UsersSection from '../pages/admin/UsersSection.vue';
import UserDetailSection from '../pages/admin/UserDetailSection.vue';
import FleetSection from '../pages/admin/FleetSection.vue';
import MachineDetailSection from '../pages/admin/MachineDetailSection.vue';
import UsageSection from '../pages/admin/UsageSection.vue';
import BillingSection from '../pages/admin/BillingSection.vue';
import CostsSection from '../pages/admin/CostsSection.vue';
import ServerStatusSection from '../pages/admin/ServerStatusSection.vue';
import ConnectionsSection from '../pages/admin/ConnectionsSection.vue';
import JobsSection from '../pages/admin/JobsSection.vue';
import JobDetailSection from '../pages/admin/JobDetailSection.vue';
import InfraSection from '../pages/admin/InfraSection.vue';
import ProgramDetailSection from '../pages/admin/ProgramDetailSection.vue';
import InfraResourceSection from '../pages/admin/InfraResourceSection.vue';
import CdKeysSection from '../pages/admin/CdKeysSection.vue';
import DataSetsSection from '../pages/admin/DataSetsSection.vue';
import AddressesSection from '../pages/admin/AddressesSection.vue';
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
    { path: '/lobby/:id(\\d+)', component: LobbyServerPage, meta: { title: 'Server' } },
    { path: '/lobbies', redirect: '/lobby' },
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
        { path: 'servers', component: ServersPage, meta: { title: 'Servers' } },
        { path: 'servers/new', component: NewServerPage, meta: { title: 'New server' } },
        {
          path: 'servers/:id(\\d+)',
          component: ServerDetailLayout,
          props: { scope: 'owner' },
          meta: { title: 'Server' },
          children: [
            { path: '', component: ServerOverviewTab },
            { path: 'stats', component: ServerStatsTab },
            { path: 'console', component: ServerConsoleTab },
            { path: 'settings', component: ServerSettingsTab },
          ],
        },
        // Stripe's Checkout and billing portal return here
        // (?checkout=success&session_id=..., ?checkout=cancel).
        { path: 'support', component: SupportTierPage, meta: { title: 'Support tier' } },
        // The billing portal's cancel flow for an address returns here.
        { path: 'addresses', component: AddressesPage, meta: { title: 'Static addresses' } },
        { path: 'account', component: AccountPage, meta: { title: 'Account' } },
        // The hosted server's page before an account could have several:
        // old links land on the list.
        { path: 'server', redirect: '/me/servers' },
      ],
    },
    {
      path: '/admin',
      component: AdminLayout,
      meta: { wide: true },
      children: [
        { path: '', redirect: '/admin/overview' },
        { path: 'overview', component: OverviewSection, meta: { title: 'Admin: Overview' } },
        { path: 'users', component: UsersSection, meta: { title: 'Admin: Users' } },
        { path: 'users/:id(\\d+)', component: UserDetailSection, meta: { title: 'Admin: User' } },
        { path: 'fleet', component: FleetSection, meta: { title: 'Admin: Fleet' } },
        { path: 'fleet/machines/:id(\\d+)', component: MachineDetailSection, meta: { title: 'Admin: Machine' } },
        { path: 'addresses', component: AddressesSection, meta: { title: 'Admin: Static addresses' } },
        {
          path: 'fleet/:id(\\d+)',
          component: ServerDetailLayout,
          props: { scope: 'admin' },
          meta: { title: 'Admin: Server' },
          children: [
            { path: '', component: ServerOverviewTab },
            { path: 'stats', component: ServerStatsTab },
            { path: 'console', component: ServerConsoleTab },
            { path: 'game-settings', component: ServerSettingsTab },
            { path: 'usage', component: ServerUsageTab },
            { path: 'logs', component: ServerLogsTab },
            { path: 'machine', component: ServerMachineTab },
          ],
        },
        { path: 'usage', component: UsageSection, meta: { title: 'Admin: Server usage' } },
        { path: 'billing', component: BillingSection, meta: { title: 'Admin: Billing' } },
        { path: 'costs', component: CostsSection, meta: { title: 'Admin: AWS costs' } },
        { path: 'service', component: ServerStatusSection, meta: { title: 'Admin: NovaWorld service' } },
        { path: 'sessions', component: ConnectionsSection, meta: { title: 'Admin: NovaWorld sessions' } },
        { path: 'jobs', component: JobsSection, meta: { title: 'Admin: Background jobs' } },
        { path: 'jobs/:id(\\d+)', component: JobDetailSection, meta: { title: 'Admin: Job' } },
        { path: 'infrastructure', component: InfraSection, meta: { title: 'Admin: Infrastructure' } },
        { path: 'infrastructure/programs/:name', component: ProgramDetailSection, meta: { title: 'Admin: Program' } },
        {
          path: 'infrastructure/resources/:id(rds|cache|nlb)',
          component: InfraResourceSection,
          meta: { title: 'Admin: AWS resource' },
        },
        { path: 'cd-keys', component: CdKeysSection, meta: { title: 'Admin: CD keys' } },
        { path: 'data-sets', component: DataSetsSection, meta: { title: 'Admin: Game data' } },
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
  // header and the tabs stay put), as does a change of the jobs page's tab or
  // filter (its query); any other navigation starts at the top.
  scrollBehavior(to, from, savedPosition) {
    if (savedPosition) return savedPosition;
    if (to.hash) return { el: to.hash, top: 72 };
    if (to.path === '/admin/jobs' && from.path === '/admin/jobs') return false;
    const onServerPage = to.matched.some((record) => record.components?.default === ServerDetailLayout);
    if (onServerPage && to.params.id === from.params.id) return false;
    return { top: 0 };
  },
});

router.beforeEach(requireUser);

// Each page names itself in the tab; the home page keeps the site's title.
const SITE_TITLE = document.title;
router.afterEach((to) => {
  const title = to.meta.title as string | undefined;
  document.title = title ? `${title} | OpenNova` : SITE_TITLE;
});

export default router;
