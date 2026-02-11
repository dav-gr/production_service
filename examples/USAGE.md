Pub/Sub Example Client — Usage

**?? IMPORTANT: Port Configuration**
The `production_service` runs **two separate TCP servers**:
- **Port 8080** (default): Request/Response server for login/validate/process commands
- **Port 9000** (default): Pub/Sub event publisher for subscribe/event delivery

**Always use port 9000** (or your configured `PubSubPort`) when running the example client.

---

**Quick Start (TL;DR)**
```bash
# 1. Build the example client
cmake --build out/build --config Debug --target pubsub_example_client

# 2. Run production_service (both servers start automatically)
./out/build/x64-debug/Debug/production_service.exe

# 3. In another terminal, run the example client
./out/build/x64-debug/Debug/pubsub_example_client.exe --port 9000
```

---

This document shows concrete examples for running the `pubsub_example_client` (in
`examples/`) against the `production_service` pub/sub implementation.

Prerequisites
- Build the project with CMake (Visual Studio generator) — the `pubsub_example_client`
  target is created by the top-level `CMakeLists.txt`.
- Ensure the database migrations are applied (run `migrations/003_add_event_subscription.sql`).
- Start `production_service` (publisher) and make sure it is listening on the pub/sub port
  (default in docs: 9100). If using native LISTEN/NOTIFY, ensure libpq is available; otherwise
  the publisher uses a polling fallback.

Building the example client (Windows / Visual Studio)
1. Configure and generate with CMake (adjust generator and paths if needed):

   cmake -S . -B out/build -G "Visual Studio 17 2022" -A x64
   cmake --build out/build --config Debug --target pubsub_example_client

2. The built executable will be in `out/build/Debug/pubsub_example_client.exe`.

Basic CLI usage

Run: `pubsub_example_client --host <host> --port <port> [options]`

Common options
- `--host` (-H)        Publisher address (default: 127.0.0.1)
- `--port` (-p)        Publisher port (default: 9000) ? **Pub/Sub port, not the request/response port**
- `--mode` (-m)        `permanent` | `one_shot` (default: `permanent`)
- `--sub` (-s)         `full` | `notify_only` (default: `full`)
- `--id` (-i)          client_id (default: `example_client`)
- `--resume` (-r)      resume_from event id (default: 0)
- `--callback-port` (-c) local port for one_shot callbacks (default: 9001)

**Important:** The pub/sub port is separate from the main request/response server port.
By default:
- Request/Response server (login, validate, process): port **8080**
- Pub/Sub event publisher (subscribe, events): port **9000**

Examples — Permanent mode (full payload)

**Quick start:**
```bash
pubsub_example_client --host 127.0.0.1 --port 9000 --mode permanent --sub full
```

1) Start a permanent client that receives full payloads and acknowledges events:

   pubsub_example_client --host 127.0.0.1 --port 9000 --mode permanent --sub full --id production_line_01

Expected behavior:
- Client connects to publisher and sends subscribe JSON:
  {"cmd":"subscribe","client_id":"production_line_01","connection_mode":"permanent","subscription_mode":"full","resume_from":0,"callback_host":null}
- Publisher responds with status ok.
- Client logs each incoming `event` message (JSON per line) and sends ACK:
  {"cmd":"ack","client_id":"production_line_01","ack":<event_id>}
- On server `ping`, client replies with `{"cmd":"pong"}`.
- To resume from a known event id N on reconnect, start the client with `--resume N`.

Example output snippet (client):

[12:00:00.123] [INF] <<< Subscription active — waiting for events...
[12:00:05.456] [INF] << EVENT #101 [items.insert] at 2026-02-10T14:23:45Z
  -> [items] INSERT row_id=99 bar_code=ABC123
[12:00:05.457] [DBG] >> ACK event_id: 101

Examples — One-shot mode (notify_only)

1) Start a one-shot client which registers a callback port and accepts deliveries:

pubsub_example_client --host 127.0.0.1 --port 9000 --mode one_shot --sub notify_only --callback-port 9001 --id production_line_cb

What happens:
- Client opens a local TCP server on the callback port (9001) and sends the subscribe
  request to the publisher with `callback_host` set to `ip:9001`.
- For each event, the publisher makes a short-lived TCP connection to the callback
  host and sends 1 JSON line (the `event`). The client must reply with an ACK on
  the same connection and then the connection is closed by the publisher.

Subscribe JSON (sent when registering):
{"cmd":"subscribe","client_id":"production_line_cb","connection_mode":"one_shot","subscription_mode":"notify_only","resume_from":0,"callback_host":"<ip>:9001"}

Event delivery example (server ? client callback connection):
{"cmd":"event","event_id":150,"table":"boxes","type":"status_to_0","row_id":23,"payload":{"id":23},"created_at":"2026-02-10T14:30:00Z"}

Client ACK response (same socket):
{"cmd":"ack","client_id":"production_line_cb","ack":150}

Notes on callback_host address
- The callback_host value should be an IP address reachable from the publisher host.
- If the client is behind NAT, ensure port forwarding or use a routable address.

Resume and ACK semantics
- ACK is monotonic: ack N implies all events ≤ N are processed by the client.
- Publisher persists ACKs in `subscribers.last_event_id` so resume_on_reconnect works.
- To resume from a particular event ID, start client with `--resume <event_id>`.

**Batched ACK Optimization (NEW)**
- The client implements batched ACK to optimize network traffic during high-volume event replay.
- Instead of sending one ACK per event (e.g., 24,964 individual ACKs), the client batches ACKs.
- Default batch size: 100 events (configurable via `ACK_BATCH_SIZE` in `client.h`)
- Behavior:
  * Collects event IDs in memory as events arrive
  * Sends a single ACK with the highest event ID every 100 events
  * Flushes remaining ACKs when read buffer is exhausted
  * Flushes all pending ACKs before disconnect
- Result: **99% reduction in ACK messages** (24,964 events → ~250 ACK messages)
- The server still receives the correct `last_event_id` for resume capability.

Example batched ACK log output:
```
>> ACK batch up to event_id: 100 (100 events)
>> ACK batch up to event_id: 200 (100 events)
...
>> ACK batch up to event_id: 24964 (64 events)
```

Bulk import behavior
- For bulk imports (DbService::doImport), the importer sets `app.bulk_import = 'on'`
  in the DB session; per-row triggers are suppressed and a single `bulk_import_finished`
  event is emitted at the end. Use `--resume` to pick up from a known event after
  a long import if needed.

Troubleshooting
- If the client never receives events:
  * Verify the publisher is running and connected to the same database.
  * Check that `event_log` rows are created by DB triggers on items/boxes changes.
  * If using one_shot mode, verify the publisher can reach the callback_host:port.
- If using native LISTEN/NOTIFY, ensure libpq is available to the publisher (or it will
  run in polling mode).

Development notes
- The example client is intentionally simple: it logs events, sends ACKs and demonstrates
  both delivery modes. For production clients, implement idempotency and deduplication
  based on `event_id` and persist last processed event to recover across restarts.

Contact
- For repository-specific questions, reference the `pub_sub_architecture_implementation.md`
  for protocol details and the `migrations/003_add_event_subscription.sql` migration.
