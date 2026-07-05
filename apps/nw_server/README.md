# nw-server — dev/golden-harness in-match host

A thin headless C++ binary that stands up the `libs/npruntime` in-match host
(the witnessed listen-server shape, a real UDP socket, the original 62 Hz
cadence) so the net iteration harness can produce golden-comparable sessions:
host a mission, let opennova or retail clients join, capture with dumpcap, and
diff against the retail golden. All protocol, crypto, and framing live in the
libs; this binary owns only the socket and the cadence.

It is a development and golden-harness tool. It is **never packaged or
shipped**: dedicated hosting for players is a serve mode of `opennova.exe`,
not a third product
([ADR 0015](../../docs/adr/0015-two-products-serve-mode.md)).

- The runtime it drives: [`libs/npruntime/ROADMAP.md`](../../libs/npruntime/ROADMAP.md)
  (build plan + live status).
- The capture/diff loop it exists for: [`scripts/net/README.md`](../../scripts/net/README.md).
