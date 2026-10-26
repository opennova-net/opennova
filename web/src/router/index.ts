import { createRouter, createWebHistory } from 'vue-router';
import LandingPage from '../pages/LandingPage.vue';
const PlayPage = () => import('../pages/PlayPage.vue');
const DownloadsPage = () => import('../pages/DownloadsPage.vue');
const AccountOverviewPage = () => import('../pages/account/AccountOverviewPage.vue');
const LoginPage = () => import('../pages/LoginPage.vue');
const RegisterPage = () => import('../pages/RegisterPage.vue');
const ForgotPasswordPage = () => import('../pages/ForgotPasswordPage.vue');
const ResetPasswordPage = () => import('../pages/ResetPasswordPage.vue');
const LobbyPage = () => import('../pages/LobbyPage.vue');
const LobbyServerPage = () => import('../pages/LobbyServerPage.vue');
const NovaLogicServerPage = () => import('../pages/NovaLogicServerPage.vue');
const AccountLayout = () => import('../pages/account/AccountLayout.vue');
const ServersPage = () => import('../pages/account/ServersPage.vue');
const NewServerPage = () => import('../pages/account/NewServerPage.vue');
const AccountPage = () => import('../pages/account/AccountPage.vue');
const SupportTierPage = () => import('../pages/account/SupportTierPage.vue');
const AddressesPage = () => import('../pages/account/AddressesPage.vue');
const SupportPage = () => import('../pages/SupportPage.vue');
const NotFoundPage = () => import('../pages/NotFoundPage.vue');
const TermsPage = () => import('../pages/legal/TermsPage.vue');
const PrivacyPage = () => import('../pages/legal/PrivacyPage.vue');
const RefundsPage = () => import('../pages/legal/RefundsPage.vue');
const ServerDetailLayout = () => import('../pages/servers/ServerDetailLayout.vue');
const ServerOverviewTab = () => import('../pages/servers/ServerOverviewTab.vue');
const ServerConsoleTab = () => import('../pages/servers/ServerConsoleTab.vue');
const ServerStatsTab = () => import('../pages/servers/ServerStatsTab.vue');
const ServerSettingsTab = () => import('../pages/servers/ServerSettingsTab.vue');
const ServerMachineTab = () => import('../pages/servers/ServerMachineTab.vue');
const ServerUsageTab = () => import('../pages/servers/ServerUsageTab.vue');
const ServerLogsTab = () => import('../pages/servers/ServerLogsTab.vue');
const AdminLayout = () => import('../pages/admin/AdminLayout.vue');
const OverviewSection = () => import('../pages/admin/OverviewSection.vue');
const UsersSection = () => import('../pages/admin/UsersSection.vue');
const UserDetailSection = () => import('../pages/admin/UserDetailSection.vue');
const FleetSection = () => import('../pages/admin/FleetSection.vue');
const MachineDetailSection = () => import('../pages/admin/MachineDetailSection.vue');
const UsageSection = () => import('../pages/admin/UsageSection.vue');
const BillingSection = () => import('../pages/admin/BillingSection.vue');
const CostsSection = () => import('../pages/admin/CostsSection.vue');
const NovaWorldSection = () => import('../pages/admin/NovaWorldSection.vue');
const JobsSection = () => import('../pages/admin/JobsSection.vue');
const JobDetailSection = () => import('../pages/admin/JobDetailSection.vue');
const InfraSection = () => import('../pages/admin/InfraSection.vue');
const ProgramDetailSection = () => import('../pages/admin/ProgramDetailSection.vue');
const InfraResourceSection = () => import('../pages/admin/InfraResourceSection.vue');
const CdKeysSection = () => import('../pages/admin/CdKeysSection.vue');
const GamesSection = () => import('../pages/admin/GamesSection.vue');
const GameSection = () => import('../pages/admin/GameSection.vue');
const AddressesSection = () => import('../pages/admin/AddressesSection.vue');
import { requireUser } from './guards';

// A page only a logged-in visitor may open sets `meta: { requiresUser: true }`
// (guards.ts). /admin does not: its AdminGate also takes the Bearer token.
const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', component: LandingPage },
    { path: '/play', component: PlayPage, meta: { title: 'Play' } },
    { path: '/downloads', component: DownloadsPage, meta: { title: 'Downloads' } },
    { path: '/login', component: LoginPage, meta: { title: 'Log in' } },
    { path: '/register', component: RegisterPage, meta: { title: 'Register' } },
    { path: '/forgot', component: ForgotPasswordPage, meta: { title: 'Forgot password' } },
    // The link a password reset mail carries: /reset?token=<64 hex digits>.
    { path: '/reset', component: ResetPasswordPage, meta: { title: 'Reset password' } },
    { path: '/lobby', component: LobbyPage, meta: { title: 'Servers' } },
    { path: '/lobby/:id(\\d+)', component: LobbyServerPage, meta: { title: 'Server' } },
    // A server on NovaLogic's NovaWorld, from the service's copy of their list.
    { path: '/lobby/novalogic/:id(\\d+)', component: NovaLogicServerPage, meta: { title: 'Server' } },
    { path: '/lobbies', redirect: '/lobby' },
    { path: '/support', component: SupportPage, meta: { title: 'Hosting and support' } },
    { path: '/terms', component: TermsPage, meta: { title: 'Terms of service' } },
    { path: '/privacy', component: PrivacyPage, meta: { title: 'Privacy notice' } },
    { path: '/refunds', component: RefundsPage, meta: { title: 'Refund policy' } },
    // The account area.
    {
      path: '/me',
      component: AccountLayout,
      meta: { requiresUser: true },
      children: [
        { path: '', component: AccountOverviewPage, meta: { title: 'Your overview' } },
        { path: 'servers', component: ServersPage, meta: { title: 'Servers' } },
        { path: 'servers/new', component: NewServerPage, meta: { title: 'New server' } },
        {
          path: 'servers/:id(\\d+)',
          component: ServerDetailLayout,
          props: { scope: 'owner' },
          meta: { title: 'Server', serverDetail: true },
          children: [
            { path: '', component: ServerOverviewTab },
            { path: 'stats', component: ServerStatsTab },
            { path: 'console', component: ServerConsoleTab },
            { path: 'settings', component: ServerSettingsTab },
          ],
        },
        // Stripe's Checkout and billing portal return here
        // (?checkout=success&session_id=..., ?checkout=cancel).
        { path: 'support', component: SupportTierPage, meta: { title: 'Billing' } },
        // The billing portal's cancel flow for an address returns here.
        { path: 'addresses', component: AddressesPage, meta: { title: 'Static addresses' } },
        { path: 'account', component: AccountPage, meta: { title: 'Account' } },
        // The hosted server's page before an account could have several:
        // old links land on the list.
        { path: 'server', redirect: '/me/servers' },
      ],
    },
    // The admin, in four areas (utils/adminNav.ts): NovaWorld, Hosting, People
    // and money, Operations. Its paths stay flat: the attention list's links
    // and the toolbox name them.
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
          meta: { title: 'Admin: Server', serverDetail: true },
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
        { path: 'billing', component: BillingSection, meta: { title: 'Admin: Subscriptions' } },
        { path: 'costs', component: CostsSection, meta: { title: 'Admin: AWS costs' } },
        { path: 'novaworld', component: NovaWorldSection, meta: { title: 'Admin: NovaWorld' } },
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
        { path: 'games', component: GamesSection, meta: { title: 'Admin: Games' } },
        // A game by its project key, or by its id when it has none.
        { path: 'games/:ref', component: GameSection, meta: { title: 'Admin: Game' } },
        // The sections' names before the admin pages were rebuilt, and the
        // NovaWorld service's and sessions' pages before they were one: old
        // links land on their new pages.
        { path: 'hosted', redirect: '/admin/fleet' },
        { path: 'service', redirect: '/admin/novaworld' },
        { path: 'server', redirect: '/admin/novaworld' },
        { path: 'sessions', redirect: { path: '/admin/novaworld', hash: '#sessions' } },
        { path: 'connections', redirect: { path: '/admin/novaworld', hash: '#sessions' } },
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
    if (to.path === from.path) return false;
    const onServerPage = to.matched.some((record) => record.meta.serverDetail);
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
