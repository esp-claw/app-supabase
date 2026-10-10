# app-supabase

esp-claw WASM app `sb`: read Supabase project status through the Management API and the device's `supabase_mgmt` credential profile. The app never sees the token; the firmware adds it to each request. The app is read-only.

## Tools

| Tool | Purpose |
|---|---|
| `sb_projects()` | Projects: name, ref, region, status, Postgres version |
| `sb_health(ref)` | Status of auth, db, realtime, rest and storage (`GET /v1/projects/{ref}/health`) |
| `sb_usage(ref)` | Disk used/size/available (`/config/disk/util`), disk config (`/config/disk`) and API request counts for the last 24 h (`/analytics/endpoints/usage.api-counts?interval=1day`) |
| `sb_branches(ref)` | Database branches: name, git branch, default, persistent, status, branch ref, updated |
| `sb_logs(ref, source, minutes=60, contains?, limit=25)` | Latest log lines of one source: `api`, `postgres`, `auth`, `functions`, `functions_edge`, `realtime`, `storage`, `rest` or `pooler` |

`ref` is the 20-letter project ref from `sb_projects`. Results are compact JSON (`cols` plus `rows`, trimmed fields).

## Device profile

Add this entry to `api_profiles` in the device config. The token is a Supabase **personal access token** (Dashboard, Account, Access Tokens), not a project API key.

```json
{"name": "supabase_mgmt", "base_url": "https://api.supabase.com", "hosts": ["api.supabase.com"],
 "auth": {"type": "bearer", "token": "<SUPABASE_PERSONAL_ACCESS_TOKEN>"}}
```

## Build

```sh
git clone --recursive https://github.com/esp-claw/app-supabase.git
cd app-supabase && ./sdk/build-app.sh .
```

The build needs clang with the wasm32 target and wasm-ld (Debian/Ubuntu: `apt install clang lld`). It writes `build/app.wasm` (about 19 KB) and `build/manifest.json` (64 KB memory, 15 s per call). CI builds every push and attaches both files to releases.

## Install

Copy both files to `/apps/sb/` on the device, for example over WebDAV (`dav://<device>/dav/apps/sb/`). Then send `/apps reload sb` in Telegram, or restart the device.

## Status

Runtime v1 in [esp-claw/esp-claw](https://github.com/esp-claw/esp-claw). The app passes a host harness (WAMR with mocked host functions and hand-written fixtures) but has not run against the live API or on a device yet. Packages are not signed yet.

Checked against the Management API OpenAPI spec (`api.supabase.com/api/v1-json`, October 2026), but not against the live API:
- **Usage:** the Management API has no plan-quota or billing-usage endpoint. `sb_usage` reports disk utilization, disk config and API request counts instead. Database size would need SQL through `POST /v1/projects/{ref}/database/query`. The firmware would ask to confirm that POST, so the app does not use it.
- **Logs:** `logs.all` has been removed (410). The app uses `GET /v1/projects/{ref}/analytics/endpoints/logs` with ClickHouse SQL on the unified `logs` table, filtering by `source_name`, over at most 24 h. Two things are unconfirmed for this endpoint: the full list of `source_name` values (the app uses the log explorer names `edge_logs`, `postgres_logs`, `auth_logs`, `function_logs`, `function_edge_logs`, `realtime_logs`, `storage_logs`, `postgrest_logs`, `supavisor_logs`) and the format of `timestamp`. The app accepts epoch microseconds or a date string.
- **Branches:** `status` is deprecated in the API and may be empty. Branching needs a paid plan; otherwise the API returns an error, and the app shows it.

Part of [esp-claw (community)](https://github.com/esp-claw), an unofficial community fork of espressif/esp-claw. It is not affiliated with Espressif.

## License

Apache-2.0
