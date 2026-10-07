# IW4x DHT Server List Integration Plan

Goal: replace IW4x's master-server communication (client list fetch from
`master.iw4x.io`, server registration) with listing via the gosrv DHT
server list, delivered as a patch against
[iw4x-client](https://github.com/iw4x/iw4x-client).

Status: APPROVED with decisions from section 7 - embedded announce in v1,
master fallback kept (dvar), patch delivered as a GitHub fork of
iw4x-client with a `dht-serverlist` branch.

## 1. What was found in the IW4x codebase

IW4x is a **32-bit DLL injected into the original `iw4m.exe`** (premake:
`architecture "x86"`, `platforms "Win32"`, C++20, SharedLib). Client and
dedicated server come from the **same tree** (`src/Components/Modules/`);
there is no separate `iw4x-server` repository.

### Client side: how the master server is actually used

`src/Components/Modules/ServerList.cpp`, `ServerList::Refresh()` online
branch (~line 420):

- Single HTTP GET: `http://master.iw4x.io/v1/servers/iw4x?protocol=0x99`
  (via `Utils::WebIO` on a detached `std::jthread`).
- Response is JSON: `{"servers":[{"ip","port","protocol"},...]}`.
- `ParseNewMasterServerResponse` filters `protocol == PROTOCOL`
  (`#define PROTOCOL 0x99` in `src/Game/Structs.hpp`) and calls
  `InsertRequest(addr)` per entry.
- `InsertRequest` queues the address; the browser's frame loop then does a
  direct UDP getchallenge/serverinfo exchange with **each server**, and
  `Insert(addr, info)` fills the visible row (hostname, map, gametype,
  clients, maxclients, password, ...).

Key consequence: **the master response only carries address hints.**
Everything shown in the browser already comes from a direct query to the
server. A gosrv DHT browse result (host, port, signed record) can feed
`InsertRequest` with zero changes to the display path - exactly the
"index is a hint, the server is the truth" design of the gosrv system.

Existing fallbacks that stay as-is: the P2P **Node system**
(`src/Components/Modules/Node.cpp`, triggered when the master response
fails) and LAN discovery.

### Server side: how servers get listed today

`src/Components/Modules/Dedicated.cpp`, `Dedicated::Heartbeat()`
(scheduled every 2 minutes, ~line 287):

- UDP `heartbeat IW4` command to the legacy engine master
  (`com_masterServerName:com_masterPort` dvars).
- The `master.iw4x.io` list itself is maintained by external IW4x
  infrastructure; the client binary is the only code that touches it.

### Feasibility gates (verified)

| Gate | Result |
|---|---|
| gosrv C ABI from a 32-bit process | `gosrv.dll` builds as **PE32 x86** with Go 1.27.1 + `i686-w64-mingw32-gcc` (c-shared, verified locally) |
| One client per process contract | Fine: one game process = one browser = one gosrv client |
| Blocking `sl_new`/`sl_browse` | Fine: IW4x already runs blocking network fetches on detached `std::jthread`s and marshals results to the client pipeline via `Scheduler::Once` |
| Log noise | Fine: `sl_log_set_cb` exists; route into IW4x's `Logger` |
| NAT | No change: outbound-only TCP/UDP like today's HTTP+UDP master |
| Building the patch here | **Not possible**: iw4x.dll needs Windows + VS + MW2 rawfiles. The patch is verified by review + the 32-bit gosrv.dll build; in-game testing happens on a Windows machine |

## 2. Design

### 2.1 Client patch (iw4x-client, new module + one integration point)

New `src/Components/Modules/Gosrv.cpp/.hpp` (component registered in
`Loader.cpp` alongside `ServerList`):

1. **Load the library**: `LoadLibrary("gosrv.dll")` (shipped next to the
   game, like `iw4x.dll`) + `GetProcAddress` into a function-pointer
   struct. Runtime load keeps the premake build unchanged and lets the
   launcher manage the DLL version. All calls guarded; a missing or
   failing library disables the DHT list and falls back to the master.
2. **Init**: `sl_log_set_cb` -> `Logger`; `sl_new` on a worker thread
   (blocking) with config from dvars (below), `join_mode` 1 by default
   (anchor chain) or 0 (direct bootstrap).
3. **Browse**: in `ServerList::Refresh()`'s online branch, *before* the
   master HTTP fetch, run `sl_browse` (version = dvar, default `"0x99"`)
   on a worker thread; each returned `SlServer` (host, port) becomes an
   `InsertRequest`. If the DHT list is non-empty, skip the master fetch
   (unless fallback dvar says otherwise); if empty/error, fall through to
   the existing master HTTP path, then to the node system. No other
   changes to the query/display pipeline.

Dvars (all `DVAR_ARCHIVE`):

| dvar | default | meaning |
|---|---|---|
| `gosrv_enable` | `1` | master switch for the DHT list |
| `gosrv_join_mode` | `1` | 0 direct bootstrap, 1 anchor chain |
| `gosrv_bootstrap` | empty | own-DHT bootstrap addrs (mode 0); `/p2p/` suffix required |
| `gosrv_anchor_bootstrap` | empty | anchor-DHT addrs (mode 1); `/p2p/` suffix required |
| `gosrv_op_key` | empty | operator public key (hex, 64 chars) |
| `gosrv_version` | `1.0` | exact-match version; must be dotted numeric (MAJOR.MINOR), e.g. `1.0`, `1.0.0` |
| `gosrv_browse_timeout_ms` | `8000` | browse budget (500-30000) |
| `gosrv_master_fallback` | `1` | also query master.iw4x.io and merge |

The compiled-in defaults (anchor addrs, operator key) are constants in the
patch; the launcher/IW4x infrastructure owns the anchor DHT and ships the
values.

### 2.2 Server listing

**Option B - embedded announce (CHOSEN for v1):** add an `sl_announce`
C API to `pkg/capi` and embed the 32-bit `gosrv.dll` in the dedicated
server, replacing `Dedicated::Heartbeat()` with the gosrv announce loop.

Design: after `sl_new` (join mode 0, own-DHT bootstrap), the server
calls `sl_announce(handle, const SlServerConfig*)`:

```c
typedef struct {
  const char *key_file;   /* ed25519 PEM; created if missing (0600) */
  const char *host;       /* public address clients can reach        */
  unsigned     port;      /* game port                                */
  const char *version;    /* exact-match version ("0x99")             */
  const char *name;       /* display name                             */
  const char **meta_keys; /* NULL-terminated, parallel to meta_vals   */
  const char **meta_vals; /* string values only in v1                 */
  unsigned  refresh_ms;   /* 0 = default R (60s)                      */
} SlServerConfig;

int sl_announce(void *h, const SlServerConfig *cfg);
```

- Reuses `internal/announce` (the same loop `serverlistd` uses); the
  record's dynamic fields (clients, map, gametype) are NOT re-pushed -
  the browser re-queries each server directly, so static announce data
  is enough for v1 (same reasoning as the sidecar option).
- `Dedicated::Heartbeat()`'s 2-minute UDP heartbeat to the legacy master
  is replaced by a one-time `sl_announce` at server init (the Go loop
  owns refresh); the legacy heartbeat is dropped.
- Server public address: dvar `gosrv_host` (empty = best-effort
  auto-detect), port from the server's listen port.

**Sidecar remains available** (unchanged `serverlistd`) for operators who
prefer it; both paths publish identical records.

**v1.1 (optional, sidecar only):** `serverlistd --query-local
<ip:port>` polls the local game port with getchallenge/serverinfo and
pushes live fields (clients, maxclients, mapname, gametype, hostname)
into the record's `meta` map each refresh cycle, so browsers can later
render live rows straight from the DHT without per-server queries.

### 2.3 Trust model (unchanged from the gosrv design)

Open: any server operator may announce with their own key; records are
ed25519-signed; the client filters by exact version match and always
verifies the server directly. The DHT provides availability, not trust -
a hostile party can list bogus addresses, but the direct query is the
final gate (same exposure as today's open master list, minus the single
point of failure).

## 3. Repository changes on the gosrv side

1. `pkg/capi`: `sl_announce` C API (see 2.2) + Go-side wiring into
   `internal/announce`; C++ smoke gains an announce mode (a client
   announces into a router-only localnet and browses it back).
2. `scripts/build-capi.sh`: new `windows32` target (GOARCH=386,
   i686-w64-mingw32-gcc) + `Makefile capi-windows32` + CI step in the
   capi job (the ubuntu image's `mingw-w64` package includes the 32-bit
   target).
3. `docs/iw4x-integration.md`: operator runbook - running the anchor DHT,
   choosing the version string, embedded server config dvars, launcher
   packaging of `gosrv.dll`, dvar reference, troubleshooting.
4. `third_party/iw4x/`: the exported patch files (`git format-patch`
   from the fork's `dht-serverlist` branch) + a README describing how to
   apply them to an iw4x-client checkout.

## 4. The IW4x patch itself (branch `dht-serverlist`)

| File | Change |
|---|---|
| `src/Components/Modules/Gosrv.hpp` (new) | function-pointer table, init/shutdown, `Browse(version, cb)`, dvar declarations |
| `src/Components/Modules/Gosrv.cpp` (new) | LoadLibrary/GetProcAddress, sl_new on worker thread, log bridge, browse wrapper, dvars |
| `src/Components/Loader.cpp` | register the component |
| `src/Components/Modules/ServerList.cpp` | online branch of `Refresh()`: DHT browse first, master fallback behind dvar |
| premake5.lua | unchanged (runtime DLL load) |

Estimate: ~600-800 lines including headers and comments; the
ServerList.cpp delta is small (one guarded block replacing the fetch
ordering).

## 5. Verification plan

1. **Buildable here**: `make capi-windows32` (PE32 `gosrv.dll`),
   `make check`, `make smoke` - CI stays green with the new target.
2. **Patch correctness**: line-by-line review against the IW4x codebase
   (threading contract, mutex discipline around `RefreshContainer`,
   scheduler pipelines). No local compile possible.
3. **In-game (Windows box, user-driven)**: apply patch, build iw4x.dll,
   run a local dedicated server + sidecar + client on loopback; verify
   the server appears via the DHT list, that `gosrv_enable 0` restores
   the master path, and that the node fallback still works with the
   anchor DHT stopped.
4. **Churn/robustness**: kill the sidecar -> server disappears from
   browse within ~2R (already proven in gosrv tests); kill the anchor ->
   client falls back to master (migration period).

## 6. Risks and mitigations

- **32-bit Go runtime in a game process**: ~40-50 MB RSS budget,
  accepted; the DLL is only loaded when `gosrv_enable` is 1.
- **Crash sensitivity**: every gosrv call sits behind an existence +
  SEH-friendly guard; on any failure the module disables itself and the
  master path takes over.
- **Operator key in client binary**: public key only (the operator's
  signing identity); the anchor DHT still holds the signed bootstrap
  list. Same trust placement as shipping any server-address constant.
- **Version string policy**: one exact-match version per IW4x protocol;
  changing the string is a one-line dvar default + re-announce by
  operators.
- **No build verification on this machine**: mitigated by keeping the
  IW4x-side diff minimal and reviewable, and by the sidecar doing all
  server-side work in a binary we *can* build and test.

## 7. Decisions (resolved)

1. Server side: **embedded announce** (`sl_announce` C API, 32-bit
   gosrv.dll in the dedicated server). Sidecar stays available as an
   alternative.
2. Migration: `gosrv_master_fallback` defaults to 1 (DHT first, master
   merged as fallback); operators can hard-cut via dvar.
3. Patch delivery: **GitHub fork** of iw4x-client (icedream) with a
   `dht-serverlist` branch; `third_party/iw4x/` in the gosrv repo keeps
   the exported `format-patch` files for reference.
4. Default version string: `0x99` (the engine protocol constant).

## Implementation status (2026-07)

Done and pushed:

- gosrv side: icedream/go-dht-serverlist main (the sl_announce commit). sl_announce + SlServerConfig in pkg/capi; windows32 target (GOARCH=386, PE32 verified); smoke
  gains --announce mode (announce on a router-only localnet, browse
  finds exactly one server, meta round-trip); CI runs it.
- IW4x side: fork icedream/iw4x-client, branch dht-serverlist,
  commit abace255. Files:
  - src/Components/Modules/gosrv_types.h (vendored sltypes.h)
  - src/Components/Modules/Gosrv.hpp/.cpp (component; LoadLibrary
    "gosrv.dll", function-pointer table, log bridge to the console,
    Browse() and StartServer())
  - ServerList.cpp: DHT browse in the online Refresh branch before
    the master HTTP; gosrv_master_fallback keeps the legacy merge
  - Dedicated.cpp: heartbeat scheduling replaced by Gosrv::StartServer
  - Loader.cpp: component registered

Verification done without a Windows box:
- gosrv.dll builds 32-bit PE32 on this machine (Go cgo +
  i686-w64-mingw32-gcc), matching the 32-bit game process.
- Gosrv.cpp passes a full g++ -fsyntax-only check against stubs that
  mirror the IW4x APIs it uses (Dvar, Logger, Library, String,
  Dedicated, Events) and the real gosrv_types.h; no IW4x headers are
  touched by the syntax-check harness.
- Not verified: a real build of iw4x.dll (needs Windows/VS plus the
  MW2 rawfiles) and an in-game browser session.

Remaining for first field test:
1. Build the fork on Windows (premake + VS), drop gosrv.dll next to
   the game (32-bit target from this repo: make capi-windows32).
2. Point a dedicated server at the localnet: gosrv_host <public ip>,
   gosrv_bootstrap from localnet B lines (with /p2p/ suffix),
   gosrv_port 28960; confirm the client browser shows it.
3. Note on gosrv_version: it must be a dotted-numeric string
   (protocol.NormalizeVersion rejects bare "1" and hex like "0x99");
   the default is "1.0". Clients and servers must use the exact
   same string.
4. Operator infrastructure: run the anchor pair, sign the bootstrap
   list, bake DEFAULT_ANCHOR_ADDRS/DEFAULT_OPERATOR_KEY_HEX into the
   fork (Gosrv.cpp) or ship them via the launcher.
