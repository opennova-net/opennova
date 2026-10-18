// The AWS resources' charts (pages/admin/InfraResourceSection.vue): each
// resource's name, and each CloudWatch metric's title, unit and meaning
// (metrics_store.h's list). A metric kept as bytes a period (a Sum) is shown a
// second; one this table does not know is shown as a count under its own name.

import { formatByteRate, formatCount, formatMemory, formatPercent } from './format';
import { listenerLabel } from './infraView';
import type { ResourceId } from '../types/usage';

export const RESOURCE_NAMES: Record<ResourceId, string> = {
  rds: 'The database (RDS)',
  cache: 'The cache (ElastiCache)',
  nlb: 'The load balancer (NLB)',
};

export interface MetricView {
  title: string;
  description: string;
  format: (value: number) => string;
  // Bytes a period, shown a second.
  perSecond?: boolean;
  integer?: boolean;
  // The y scale's least top (100 for a percent).
  minTop?: number;
}

const percent = (title: string, description: string): MetricView => ({ title, description, format: formatPercent, minTop: 100 });
const count = (title: string, description: string): MetricView => ({ title, description, format: formatCount, integer: true });

const METRICS: Record<ResourceId, Record<string, MetricView>> = {
  rds: {
    CPUUtilization: percent('CPU', "Its instance's CPU, in percent."),
    FreeableMemory: { title: 'Free memory', description: 'Memory the database has to spare.', format: formatMemory },
    DatabaseConnections: count('Connections', 'Connections open to it.'),
    NetworkReceiveThroughput: { title: 'Network in', description: 'Bytes a second it takes in.', format: formatByteRate },
    NetworkTransmitThroughput: { title: 'Network out', description: 'Bytes a second it sends.', format: formatByteRate },
    ReadIOPS: count('Disk reads', 'Read operations a second.'),
    WriteIOPS: count('Disk writes', 'Write operations a second.'),
  },
  cache: {
    CPUUtilization: percent('CPU', "Its primary node's CPU, in percent."),
    EngineCPUUtilization: percent('Engine CPU', "The CPU its engine's own thread uses, in percent."),
    BytesUsedForCache: { title: 'Memory used', description: 'Memory its data takes.', format: formatMemory },
    DatabaseMemoryUsagePercentage: percent('Memory used of its limit', 'In percent of what it may use before it evicts keys.'),
    NetworkBytesIn: { title: 'Network in', description: 'Bytes a second it takes in.', format: formatByteRate, perSecond: true },
    NetworkBytesOut: { title: 'Network out', description: 'Bytes a second it sends.', format: formatByteRate, perSecond: true },
    CurrConnections: count('Connections', 'Clients connected to it.'),
  },
  nlb: {
    ProcessedBytes: { title: 'Traffic', description: 'Bytes a second through it, every listener together.', format: formatByteRate, perSecond: true },
    ActiveFlowCount: count('Active flows', 'Connections and UDP flows open through it.'),
  },
};

// A metric's view: the table's, a target group's healthy hosts
// ("HealthyHostCount:<listener>"), or a count under its own name.
export function metricView(resource: ResourceId, name: string): MetricView {
  const known = METRICS[resource][name];
  if (known) return known;
  if (name.startsWith('HealthyHostCount:')) {
    const listener = name.slice('HealthyHostCount:'.length);
    return count(`Healthy targets: ${listenerLabel(listener)}`, 'The least healthy targets behind this listener in each period.');
  }
  return count(name, 'As CloudWatch keeps it.');
}
