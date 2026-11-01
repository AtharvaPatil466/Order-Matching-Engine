# Operational Runbook

This runbook covers the two binaries this repository builds and ships:
`OrderEngine` (`src/main.cpp`) and `GatewayServer` (`src/gateway_main.cpp`).
It describes what they do today, including what they do not do.

**Every `bash` block below is executed.** `scripts/runbook_smoke.sh` builds both
binaries and `JournalReplayCLI` in an `ubuntu:24.04` container, puts them on
`PATH`, and runs the blocks top to bottom in one shell, as an operator working
through this page would. A block preceded by a `runbook-smoke: skip` marker is
not run, and the marker says why (it needs systemd, Docker, a real NIC, or it
is a config file or sample output rather than a command). The script prints
every block it skips. It also fails if a flag listed in a table here is not in
the `--help` of the binary that table describes.

---

## 1. What ships

| Binary | Takes client orders | Admin HTTP port | Journal | Market data |
|---|---|---|---|---|
| `OrderEngine` | No TCP order-entry port. Orders reach it only through the admin port's `/chaos/order` (off unless `OB_CHAOS_INJECT=1` and `OB_CHAOS_TOKEN` are set), or as OUCH in a build with `-DENABLE_DPDK=ON` (off by default) started with `OB_DPDK=1`. | Yes (default 8080) | Optional (`--journal`), group commit | None |
| `GatewayServer` | Yes: a TCP port (default 9876) speaking the binary protocol in `include/GatewayProtocol.h` | None | Optional (`--journal`), each order synced before its ack | Shared memory `orderbook_md`, read by `MdSubscriber orderbook_md` |

`OrderEngine` runs a multi-threaded engine (`--threads`) over `--symbols`
symbols numbered from 0. `GatewayServer` runs a single-threaded engine over
symbols 0 and 1. The two do not talk to each other: each has its own book and
its own journal.

Both binaries refuse an argument they do not understand, and a value flag with
nothing after it, before doing anything else:

<!-- runbook-smoke: skip — sample output, not a command -->
```text
[Config] FATAL: unknown argument '--lean' — refusing to start. See --help.
[Gateway] FATAL: --journal needs a value — refusing to start. See --help.
```

`--help` on either binary prints every flag it accepts, generated from the same
table the check uses. A flag always wins over its environment variable.

Both binaries write progress lines to stdout without flushing and FATAL lines
to stderr. Under systemd or with stdout redirected to a file, stdout is block
buffered, so progress lines can arrive late; the commands below run
`OrderEngine` under `stdbuf -oL` (coreutils) to get them line by line.

The commands below assume the binaries are on `PATH`. A CMake build puts
`OrderEngine`, `GatewayServer` and `MdSubscriber` in the build directory and
`JournalReplayCLI` in its `tools/` subdirectory.

---

## 2. Before the first start

### Journal storage

Put the journal on local storage: instance-store NVMe, a local SSD, or tmpfs if
losing it on a host reboot is acceptable. A network block device (EBS,
Persistent Disk, a SAN) puts a network round trip into every journal sync. On
AWS Nitro both instance-store and EBS volumes appear as `/dev/nvme*`; the
device model string tells them apart. This check prints what backs the journal
directory and exits non-zero on a network filesystem or an EBS volume:

```bash
JOURNAL_DIR=/var/lib/orderengine
mkdir -p "$JOURNAL_DIR"
SRC=$(findmnt -no SOURCE --target "$JOURNAL_DIR")
FSTYPE=$(findmnt -no FSTYPE --target "$JOURNAL_DIR")
echo "journal dir $JOURNAL_DIR: source=$SRC fstype=$FSTYPE"
case "$FSTYPE" in
  nfs*|cifs|9p) echo "FATAL: network filesystem ($FSTYPE)"; exit 1 ;;
  tmpfs|ramfs)  echo "WARN: RAM-backed, lost on host reboot" ;;
  *)
    DISK=$(lsblk -no PKNAME "$SRC" 2>/dev/null || true)
    MODEL=$(cat "/sys/block/${DISK:-none}/device/model" 2>/dev/null || true)
    case "$MODEL" in
      *"Elastic Block Store"*) echo "FATAL: EBS volume"; exit 1 ;;
      *"Instance Storage"*)    echo "OK: instance-store NVMe" ;;
      *) echo "WARN: device '${MODEL:-unknown}' not classified; confirm it is local by hand" ;;
    esac ;;
esac
```

### Upgrading onto an existing journal

Builds before `cdd45ac` seeded up to 200 synthetic Limit orders
per symbol, as participants 1 and 2, at startup, and journaled them. A journal
whose lineage began with such a build carries content epoch 0 in its header;
journals this build creates carry 1, and a checkpoint keeps the epoch of the
journal it was taken from. Both binaries refuse to replay an epoch-0 journal
that holds entries:

<!-- runbook-smoke: skip — sample output, not a command -->
```text
[Engine] FATAL: <path> may hold orders nobody sent
        (journal content epoch 0; this build writes 1). ...
```

| Situation | Action |
|---|---|
| The node never took client traffic | Move the journal aside and start fresh |
| It holds real client orders too | Inspect it (`JournalReplayCLI --journal <copy> --tail 1000` shows each entry's participant), then start with `--replay-legacy-journal` (`OB_REPLAY_LEGACY_JOURNAL=1`) and cancel the participant 1 and 2 orders by id |

Once replayed, synthetic and real orders cannot be told apart, and the journal
stays epoch 0 through every checkpoint, so that node needs the flag on every
boot from then on.

### Upgrading across a change in the order rules

Replay re-runs each journaled order through this build's book rules, not the
rules of the build that admitted it. An order this build refuses is refused
again at replay (the replay count, §7, still counts it). Since roadmap 1.6-F2
and 1.6-F6 that is:

- a `Stop`, `StopLimit` or `MIT` with `stopPrice` 0, and a `StopLimit` with
  `stopLimitPrice` 0;
- a `TrailingStop` whose trail is not inside (0, reference);
- with a config file, an order priced outside its symbol's price band (§6),
  which older builds never applied.

A history with stops, MITs, trailing stops or pegs in it can also replay to
different trades: a `Stop` now elects a market order, elections test the
sweep's low and high print, and they run after the triggering order is final
([MatchingRules.md](MatchingRules.md) §5-6). To carry the book across rather
than re-run its history, stop the old `OrderEngine` with `SIGTERM`, which
checkpoints (§5), so the new build restores the resting orders instead of
replaying the trades (`GatewayServer` does not checkpoint at shutdown). A
resting order the new rules refuse is still dropped by that restore: cancel or
replace it before the upgrade.

---

## 3. Starting OrderEngine

`OrderEngine` refuses to start without an admin token (or an explicit
`--admin-no-auth`). Without `--journal` it keeps no journal, and a restart loses
every resting order.

```bash
mkdir -p /var/lib/orderengine /etc/orderengine
export OB_ADMIN_TOKEN=replace-with-a-long-random-secret
stdbuf -oL OrderEngine --threads 2 --symbols 2 --port 8080 \
    --journal /var/lib/orderengine/journal.wal \
    > /var/lib/orderengine/engine.log 2>&1 &
echo $! > /var/lib/orderengine/engine.pid
# The admin port opens only after the journal is replayed; retry until it answers.
curl -sf --retry 50 --retry-delay 1 --retry-connrefused http://127.0.0.1:8080/readyz
grep -F "[Engine] Ready for traffic." /var/lib/orderengine/engine.log
```

### Flags

| Flag | Environment | Default | Meaning |
|---|---|---|---|
| `--threads N` | — | 4 | Worker threads |
| `--symbols S` | — | 4 | Symbols 0..S-1 |
| `--port P` | — | 8080 | Admin HTTP port |
| `--admin-bind ADDR` | `OB_ADMIN_BIND` | 127.0.0.1 | Admin listen address (IPv4). The Docker image sets 0.0.0.0. |
| `--admin-token TOKEN` | `OB_ADMIN_TOKEN` | — | Bearer token for every endpoint except `/health` and `/readyz` |
| `--admin-no-auth` | `OB_ADMIN_NO_AUTH=1` | off | Run the admin port without a token, on purpose |
| `--config PATH` | `OB_CONFIG_PATH` | — | Config file (§6). With one, every symbol needs a price band |
| `--journal PATH` | `OB_JOURNAL_PATH` | — | Journal file; replayed at boot (§7) |
| `--journal-max-mb MB` | `OB_JOURNAL_MAX_SIZE_MB` | off | Checkpoint once the journal reaches MB megabytes |
| `--replay-legacy-journal` | `OB_REPLAY_LEGACY_JOURNAL=1` | off | Replay an epoch-0 journal (§2) |
| `--session-schedule PRE,OPEN,CLOSE_AUCTION,CLOSE` | `OB_SESSION_SCHEDULE` | off | Session timetable, HH:MM UTC |
| `--role primary\|backup` | `OB_NODE_ROLE` | standalone | Replication role; refused without the next flag (§10) |
| `--allow-unsafe-replication` | `OB_ALLOW_UNSAFE_REPLICATION=1` | off | Run replication despite its known defects |
| `--node-id N` | `OB_NODE_ID` | 1 | Replication node id |
| `--repl-port P` | `OB_REPLICATION_PORT` | 9002 | Primary's replication listen port |
| `--primary-host HOST` | `OB_PRIMARY_HOST` | — | Backup only: the primary's host |
| `--primary-port P` | `OB_PRIMARY_REPLICATION_PORT` | 9002 | Backup only: the primary's port |
| `--help` | — | — | Print the flags and exit |

Other environment variables `OrderEngine` reads: `OB_CHAOS_INJECT` and
`OB_CHAOS_TOKEN` (arm `/chaos/order`, §4), every config key as `OB_<KEY>` (§6),
and in a DPDK build `OB_DPDK=1` and `OB_DPDK_PORT`.

### What happens at boot, in order

1. Arguments are checked; an unknown one is fatal.
2. The config file is loaded, if one is named. A file that cannot be read is a
   `[Config] WARNING`, and the engine goes on without it — and then stops at
   step 4, which finds no price bands (unless the environment supplies them,
   §6).
3. Startup validation: the `instrument.*` and `participant.*` keys, the journal
   path's parent directory, and a replication role without
   `--allow-unsafe-replication`. Any failure prints
   `[Config] FATAL: startup configuration is invalid` with one line per
   problem and exits 1.
4. With a config file, each symbol's price band is set from
   `instrument.<s>.min_price` / `max_price` (§6), before any order exists.
   A symbol with no band prints `[Config] FATAL: symbol <s> has no price band`
   and exits 1; a band the book cannot hold is fatal too. On success:
   `[Config] Price bands set for <S> symbols`. Without a config file every
   book centres its band on its first resting order instead.
5. The journal is opened, a torn final write is cut, and the journal is
   replayed (§7). The legacy-epoch and unreadable-journal refusals happen here.
6. The alert webhook is set up if `alert_webhook_url` is set (§6). An
   `https://` URL is refused: the dispatcher has no TLS client, so point it at
   a local TLS terminator on `http://127.0.0.1`.
7. Worker threads, the GTD/DAY expiry timer and, with `--session-schedule`,
   the session scheduler start.
8. Replication starts, if a role was allowed.
9. The admin port binds. No token and no `--admin-no-auth` is fatal, and so
   is a failed bind.
10. `[Engine] Ready for traffic.` `/readyz` answers 200 from here on.

Because the admin port opens only at step 9, a probe during replay gets
connection refused rather than 503. Give a liveness probe enough initial delay
for your replay time.

---

## 4. Health and monitoring

| Endpoint | Auth | Returns |
|---|---|---|
| `GET /health` | none | `{"status":"ok"}` |
| `GET /readyz` | none | 200 `{"status":"ready"}`, or 503 before the engine is ready |
| `GET /metrics` | token | JSON: `submitted`, `processed`, `rateLimited`, `backpressureRejected`, `queueDropped`, `pendingOrders`, `e2eLatency` percentiles in ns |
| `GET /prometheus` | token | Text exposition of the metrics registry |
| `GET /book?symbolId=N` | token | 10 levels per side and the last trade |
| `GET /auction?symbolId=N` | token | Trading state and indicative auction price, paired and imbalance quantity |
| `GET /audit?symbolId=N` | token | Best bid/ask, spread, 5 levels |
| `GET /otr?participantId=N` | token | Order and trade counts and order-to-trade ratio, **symbol 0 only** |
| `GET /fees?participantId=N` | token | Accrued fee (no fee schedule is set by this binary) |
| `GET /risk?tier=T&entityId=E` | token | Net position at a risk tier (0 Trader … 3 Firm) |
| `GET /cat` | token | Audit-record count |
| `GET /role`, `GET /replication` | token | `standalone` unless replication runs |
| `GET /journal/head` | token | Whether a journal is on and its sequence number |
| `GET /version` | token | Git SHA and build time |
| `GET /chaos/order?...` | token + `X-Chaos-Token` | Submits an order. `{"enabled":false}` unless `OB_CHAOS_INJECT=1` and `OB_CHAOS_TOKEN` are set |

Any other path returns the endpoint list. The token goes in an
`Authorization: Bearer <token>` header; without it every endpoint but `/health`
and `/readyz` answers 401.

```bash
curl -sf http://127.0.0.1:8080/health
H="Authorization: Bearer $OB_ADMIN_TOKEN"
curl -sf -H "$H" http://127.0.0.1:8080/metrics
curl -sf -H "$H" http://127.0.0.1:8080/prometheus
curl -sf -H "$H" "http://127.0.0.1:8080/book?symbolId=0"
curl -sf -H "$H" http://127.0.0.1:8080/journal/head
test "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/metrics)" = 401
```

Prometheus series `OrderEngine` can export (each appears once it is first
used): `orders_accepted_total`, `orders_rejected_total`,
`journal_entries_committed_total`, `order_pool_utilization_percent{symbol="N"}`,
and with replication `replication_entries_shipped_total`,
`replication_bytes_sent_total`, `replication_snapshot_streams_total`,
`replication_snapshot_entries_total`. `pendingOrders` and the latency
percentiles are in `/metrics` JSON only. `GatewayServer` keeps
`gateway_connections_total` and `gateway_disconnects_total` but has no port to
read them from.

<!-- runbook-smoke: skip — Prometheus configuration, not a command -->
```yaml
scrape_configs:
  - job_name: orderengine
    metrics_path: /prometheus
    authorization:
      credentials: <admin token>
    static_configs:
      - targets: ['localhost:8080']
```

---

## 5. Stopping and restarting

`SIGTERM` or `SIGINT` shuts `OrderEngine` down cleanly: the admin port closes,
new orders are refused, the queues drain, and with a journal the engine
checkpoints every resting order, DAY orders included (§7). `SIGHUP` reloads the
config (§6). No other signal is handled; anything else takes its default
action, which for most signals ends the process without the checkpoint.

```bash
PID=$(cat /var/lib/orderengine/engine.pid)
kill -TERM "$PID"
while kill -0 "$PID" 2>/dev/null; do sleep 0.2; done
grep -F "[Engine] Shutdown complete." /var/lib/orderengine/engine.log
```

A restart on the same journal replays it before the admin port opens. This one
also names a config file, used in §6. A config file must give every symbol a
price band, or the engine refuses to start:

```bash
cat > /etc/orderengine/engine.conf <<'EOF'
log_sink = stderr
instrument.0.min_price = 1
instrument.0.max_price = 200000
instrument.1.min_price = 1
instrument.1.max_price = 200000
rate_limit.default_rate = 1000
rate_limit.default_burst = 100
EOF
stdbuf -oL OrderEngine --threads 2 --symbols 2 --port 8080 \
    --journal /var/lib/orderengine/journal.wal --config /etc/orderengine/engine.conf \
    > /var/lib/orderengine/engine.log 2>&1 &
echo $! > /var/lib/orderengine/engine.pid
curl -sf --retry 50 --retry-delay 1 --retry-connrefused http://127.0.0.1:8080/readyz
grep -E "\[Engine\] Replayed [0-9]+ of [0-9]+ recoverable journal entries" /var/lib/orderengine/engine.log
```

---

## 6. Configuration and reload

The config file is `key = value` lines; `#` starts a comment. Every key can be
overridden by an environment variable `OB_<KEY>`, upper-cased with `.` and `-`
turned into `_` (`log_sink` → `OB_LOG_SINK`), and the environment wins over the
file. Keys `OrderEngine` reads:

| Key | When | Effect |
|---|---|---|
| `log_sink` | startup | `stderr` sends structured JSON logs to stderr; `null` (default) discards them |
| `alert_webhook_url`, `alert_webhook_format` (`generic`/`slack`/`pagerduty`), `alert_min_level` (`info`/`warning`/`critical`/`fatal`), `alert_routing_key` | startup | Webhook alerts; an unknown format or level, or an `https://` URL, is fatal |
| `instrument.<s>.min_price`, `instrument.<s>.max_price` | startup | Required for every symbol 0..S-1 whenever a config file is named. The symbol's price band in ticks, at most 200,000 wide, set before replay. An order or replace priced outside it is rejected with `OutOfPriceRange` before it is acked ([MatchingRules.md](MatchingRules.md) Rule 1.4) |
| `instruments`, `instrument.<id>.{tick_size,lot_size}` | startup | Validated only (with `min_price` / `max_price`, for each id listed in `instruments`) |
| `participants`, `participant.<id>.{position_limit,otr_limit}` | startup | Validated only |
| `journal_path` | startup | Validated only; the journal is `--journal` / `OB_JOURNAL_PATH` |
| `rate_limit.default_rate`, `rate_limit.default_burst` | `SIGHUP` only | Default rate limit for every participant, applied when the rate is above 0 |
| `order_pool_capacity`, `trade_history_capacity` | book creation | Book sizing. Read from the file named by `OB_CONFIG_PATH` and from `OB_<KEY>`, **not** from `--config` |

Nothing else in `config/engine.conf.example` is read by either binary.
`GatewayServer` reads no config file.

`SIGHUP` re-reads the `--config` file and applies only the rate limit. The main
loop checks for it every 100 ms:

```bash
kill -HUP "$(cat /var/lib/orderengine/engine.pid)"
sleep 1
grep -F "[Config] Rate limiter reconfigured: 1000 msg/s" /var/lib/orderengine/engine.log
```

Thread count, symbol count, ports, the journal path and the replication role
take effect only at a restart.

---

## 7. The journal

### What it holds and when it is synced

Each accepted order, cancel, modify and trading-state change is appended as a
fixed-size record.
`OrderEngine` uses group commit: appends are batched and synced together.
`GatewayServer` syncs each append before returning, and holds every event a
client can see (acks, fills, market data) until the entry behind it is on disk.

### Checkpoints

A checkpoint **replaces the journal** with what it takes to rebuild the books;
the fills and cancels before it are gone. Copy the file first if you need that
history. What it keeps:

- one snapshot record per live order: the resting orders level by level, front
  to back, so a restored level keeps its time priority, then the parked ones
  (stops, trailing stops, on-close and auction market orders). A stop keeps its
  election latch and a trailing stop the trigger its trail had ratcheted to;
- each book's session: a begun session's `PreOpen` with its trading date (0 when
  undated), a halted book's pre-halt state, and any state but `Continuous`;
- per book, the trade-id counter, the last trade price and the reference price
  (the circuit breaker's), so a restored book trips and crosses as the one that
  wrote it would have.

Checkpoints run:

- automatically at 250,000 entries or 64 MiB, in both binaries (compiled in;
  no key changes it);
- in `OrderEngine`, when `--journal-max-mb` is set and the journal reaches it;
- on every clean `OrderEngine` shutdown (§5). `GatewayServer` does not
  checkpoint at shutdown.

There is no other way to trigger one.

### Inspecting a journal

`JournalReplayCLI` reads a journal without starting either binary. Point it at
a copy, never at the file a running engine is writing:

```bash
cp /var/lib/orderengine/journal.wal /var/lib/orderengine/journal.inspect
JournalReplayCLI --journal /var/lib/orderengine/journal.inspect --stats
JournalReplayCLI --journal /var/lib/orderengine/journal.inspect --tail 20
JournalReplayCLI --journal /var/lib/orderengine/journal.inspect --dump-orders
rm /var/lib/orderengine/journal.inspect
```

`--stats` counts entries by type, `--tail N` prints the last N with symbol,
order id, participant, side, price and quantity, and `--dump-orders` replays the
copy in memory and prints the resulting book.

### What boot does with it

Both binaries run the same sequence (`include/JournalBoot.h`) and print the
result: `[Engine] Replayed X of Y recoverable journal entries`.

- **Torn final write.** Less than one record after the last whole record, which
  is what a crash partway through a write leaves, is cut at boot and appended
  to `<path>.torn`: `[Journal] Cut a torn final write: N byte(s) ...`. It cannot
  hold a record, so nothing replayable is lost, and the engine starts.
- **Anything longer** may be a corrupt record with valid ones behind it. The
  engine refuses to start (`[Journal] REFUSING TO APPEND: <path> has N byte(s)
  after its last replayable record`, then `[Engine] FATAL: journal at <path>
  could not be opened safely`), leaves the file untouched, and prints the exact
  repair: copy the file aside and `truncate -s <bytes>` it at the end of the
  last replayable record. That discards whatever was behind the damage, so
  reconcile the orders after it by hand.
- **Unreadable journals** (another format version or record size, no valid
  record at all, or a log that does not start at sequence 1) are refused the
  same way. Replay them with the build that wrote them or move them aside.
- **Legacy epoch**: §2.
- **Replayed nothing.** If the journal holds recoverable entries and replay
  applied none, the engine refuses to start rather than serve an empty book.

The replay count is entries consumed, not entries applied: an entry that
replays and is refused (a duplicate order id, for one) is still counted.

### Write failures

If a write, flush or sync of the journal fails (a full disk, for one), the
process prints `[Journal] FATAL: <what> failed for <path>` and aborts. A
restart replays up to the last whole record. Free space before restarting.

Stop the engine before the next section:

```bash
PID=$(cat /var/lib/orderengine/engine.pid)
kill -TERM "$PID"
while kill -0 "$PID" 2>/dev/null; do sleep 0.2; done
grep -F "[Engine] Shutdown complete." /var/lib/orderengine/engine.log
```

---

## 8. GatewayServer

### Flags

`GatewayServer [PORT] [options]`. `PORT` is the listen port (default 9876) and
is read only as the first argument.

| Flag | Environment | Meaning |
|---|---|---|
| `--participant-credentials FILE` | `OB_PARTICIPANT_CREDENTIALS` | Credentials, one per line as `user:secret:id[,id...]` |
| `--no-participant-auth` | `OB_NO_PARTICIPANT_AUTH=1` | Accept every claimed participant id, on purpose |
| `--journal PATH` | `OB_JOURNAL_PATH` | Journal every accepted order and replay it at start |
| `--replay-legacy-journal` | `OB_REPLAY_LEGACY_JOURNAL=1` | Replay an epoch-0 journal (§2) |
| `--help`, `-h` | — | Print the flags and exit |

`OB_ENGINE_HOST` and `OB_ENGINE_PORT`, both set and non-empty, switch to
forwarding mode (below).

It refuses to start with no credentials and no `--no-participant-auth`, and
with a credentials file that is unreadable or holds no credentials. A file
readable by group or other draws a warning.

```bash
install -m 600 /dev/null /etc/orderengine/participants.conf
echo 'firm-a:replace-with-a-secret:100,101' > /etc/orderengine/participants.conf
GatewayServer 9876 --participant-credentials /etc/orderengine/participants.conf \
    --journal /var/lib/orderengine/gateway.wal \
    > /var/lib/orderengine/gateway.log 2>&1 &
echo $! > /var/lib/orderengine/gateway.pid
for _ in $(seq 50); do grep -qF "Order Gateway listening" /var/lib/orderengine/gateway.log && break; sleep 0.2; done
grep -F "Order Gateway listening on port 9876" /var/lib/orderengine/gateway.log
```

Once a second, between orders, it runs order expiry and publishes a 5-level
snapshot of each symbol to the shared-memory feed. It logs every trade to
stdout.

A participant's `KillSwitch` message (type 5) cancels all of that
participant's resting orders. Each cancel is journaled. It is a one-shot sweep,
not a halt: the participant can send new orders straight away.

### Forwarding mode

With `OB_ENGINE_HOST` and `OB_ENGINE_PORT` set, `GatewayServer` runs no engine:
it relays each request frame to that host and port and relays the response
back. The only binary that speaks this protocol on a port is another
`GatewayServer`. The proxy does not check credentials and ignores
`--participant-credentials` and `--journal`; the gateway it forwards to does
both.

```bash
OB_ENGINE_HOST=127.0.0.1 OB_ENGINE_PORT=9876 GatewayServer 9877 \
    > /var/lib/orderengine/proxy.log 2>&1 &
PROXY=$!
for _ in $(seq 50); do grep -qF "Proxy listening" /var/lib/orderengine/proxy.log && break; sleep 0.2; done
grep -F "Proxy listening on port 9877 -> 127.0.0.1:9876" /var/lib/orderengine/proxy.log
kill -TERM "$PROXY"
```

### Stopping

`SIGTERM` or `SIGINT` stops the listener, the market-data publisher and the
engine. There is no checkpoint, so the next start replays the whole journal.

```bash
PID=$(cat /var/lib/orderengine/gateway.pid)
kill -TERM "$PID"
while kill -0 "$PID" 2>/dev/null; do sleep 0.2; done
grep -F "Shutting down..." /var/lib/orderengine/gateway.log
```

---

## 9. Incident quick reference

| Symptom | Where to look | Action |
|---|---|---|
| Process exits at start | The `FATAL` line on stderr | Each names its cause and the fix: §3 for `OrderEngine`, §8 for `GatewayServer`, §7 for the journal |
| `OrderEngine` probes refused for a long time after a restart | Journal size | Replay runs before the admin port opens; check `JournalReplayCLI --stats` on a copy |
| `[Journal] FATAL: ... failed` and an abort | Disk space, I/O errors | §7 "Write failures" |
| `[Journal] REFUSING TO APPEND` | The repair line in the message | §7; keep a copy of the file before truncating |
| Webhook alerts never arrive | Startup `[Alert]` line | Only `http://` is delivered; check the sidecar |
| A client keeps trading after a kill message | Nothing is wrong | The kill is a one-shot cancel, not a halt (§8) |

Trading behaviour (STP, the circuit breaker, auctions, order types) is in
[MatchingRules.md](./MatchingRules.md).

---

## 10. Replication

Replication between two `OrderEngine` nodes exists but is refused unless
`--allow-unsafe-replication` (`OB_ALLOW_UNSAFE_REPLICATION=1`) is passed with a
role, because of open CRITICAL defects (REPL-1..6): a deposed primary is never
fenced, a promoted backup can execute orders twice, a slow backup freezes acks,
and entries lost in a disconnect are never re-sent. A backup promotes itself
when the primary's heartbeats and lease lapse and logs
`[Replication] PROMOTED to primary — exiting replay mode`; there is no
operator step and no client failover, since `OrderEngine` has no order-entry
port. Do not rely on it for business continuity.

---

## 11. Deployment templates

### systemd

<!-- runbook-smoke: skip — a unit file for systemd, not a command -->
```ini
[Unit]
Description=Order Matching Engine
After=network.target

[Service]
Type=simple
Environment=OB_ADMIN_TOKEN=<token>
ExecStart=/usr/bin/stdbuf -oL /opt/orderengine/OrderEngine --threads 4 --symbols 4 --port 8080 --journal /var/lib/orderengine/journal.wal
Restart=on-failure
RestartSec=5
LimitNOFILE=65536
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
```

`systemctl stop` sends `SIGTERM`, which is the clean shutdown in §5.

### Docker

The image (`Dockerfile`) runs `OrderEngine --threads 4 --port 8080 --symbols 4`
and sets `OB_ADMIN_BIND=0.0.0.0`. `docker-compose.yml` adds
`OB_JOURNAL_PATH=/app/journal/primary.wal` on a volume and requires
`OB_ADMIN_TOKEN`. It mounts `config/engine.conf.example` at
`/app/config/engine.conf` but passes no `--config` or `OB_CONFIG_PATH`, so that
file is not loaded.

<!-- runbook-smoke: skip — needs a Docker daemon -->
```bash
OB_ADMIN_TOKEN=$(openssl rand -hex 32) docker compose up -d engine-primary
curl -sf http://localhost:8080/health
```

### Host tuning

Kernel isolation, IRQ affinity and CPU frequency settings are in
[OSTuning.md](./OSTuning.md). Its scripts need root on a real host with the NIC
in question:

<!-- runbook-smoke: skip — needs root on a physical host and a real NIC -->
```bash
sudo scripts/set_irq_affinity.sh <nic_dev> <housekeeping_mask>
sudo scripts/set_cpu_perf.sh
```

Clock sync is checked with your time daemon, for example `chronyc tracking`.
Sizing guidance is in [CapacityPlanning.md](./CapacityPlanning.md).
