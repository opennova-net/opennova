class_name ItemEffectDirectorStats
extends RefCounted

## Value-only census of ItemEffectDirector's per-item effect bookkeeping (the
## F3/GUT read seam, ADR 0018): how many placed nodes and static sources hold
## an attached effect group, how many wait on a hidden particle switch or on
## their controller lifecycle, and how many controller nodes are active.

var registered_nodes := 0  # placed nodes whose authored effect groups are attached
var registered_static := 0  # static sources whose effect groups are attached
var pending_nodes := 0  # nodes deferred by the hidden particle switch (retry once)
var pending_static := 0  # static sources deferred the same way
var control_nodes := 0  # PlayerControl item nodes waiting on controller edges
var control_active := 0  # identity aliases with a controlling occupant
var owner_keys := 0  # per-item owner keys with a presented node
