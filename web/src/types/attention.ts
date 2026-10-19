// What needs an admin, as GET /api/admin/attention answers it
// (apps/novaworld_common/hosted/model/fleet_health.h): the fleet's health findings
// and the deployment's firing CloudWatch alarms.

export type FindingLevel = 'danger' | 'warn' | 'info';

export interface Finding {
  level: FindingLevel;
  // What it is about ("machine 3", "server 12", "the lobby").
  subject: string;
  text: string;
  // Since when it holds; null when unknown.
  since: string | null;
  // The site page to look at (a path), or null.
  link: string | null;
  // The rows it was read from.
  source: string;
}

export interface FiringAlarm {
  name: string;
  state_since: string | null;
  description: string;
}

export interface Attention {
  // Danger first, then warn, then info, oldest first within each.
  findings: Finding[];
  alarms: FiringAlarm[];
  // False where the alarms are not read (the local stack).
  alarms_read: boolean;
  checked_at: string;
}
