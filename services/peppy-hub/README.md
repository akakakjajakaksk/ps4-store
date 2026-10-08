# Peppy Hub

Central premium catalog and administrator account service for Peppy Store. The existing packaged free catalog stays local to the store. New central games, media and themes require an active premium account. URL imports on the console can remain available to both editions; this service does not charge for or proxy those imports.

The service is a portable Cloudflare Worker with a D1 SQLite binding named `DB`. It uses WebCrypto PBKDF2-SHA256 with a random 128-bit salt and 100,000 iterations (the Worker WebCrypto limit), and random 256-bit bearer sessions. Only a session's SHA-256 is persisted. Public config, releases, game catalogs and store binaries never contain account passwords, password hashes or a user list. The hidden console administrator shortcut is a UI shortcut; every server write independently requires an administrator session.

## Run and deploy

Node 24 or later is needed for local tests/build. No npm installation is needed:

```sh
npm test
npm run build
```

`dist/worker.js` is self-contained JavaScript; alternatively a TypeScript-capable deployment can use `src/worker.ts`. Apply `migrations/0001_hub.sql` to D1 before serving requests. `wrangler.example.toml` documents binding/variable names; replace the origin/database placeholders for the actual deployment. A Sites deployment can import `db/schema.ts` into its Drizzle schema and generate its managed schema-only migrations. The Worker lazily initializes the catalog singleton during the first administrator publication; the standalone SQL migration also includes that initialization.

Configure `OWNER_USERNAME` and `OWNER_PASSWORD` through the deployment platform's **secret** facility, never in a committed `.env`, TOML, SQL, frontend or PKG. The first successful owner login creates one administrator atomically. An established administrator, including a revoked administrator, is never replaced by environment secrets. After confirming bootstrap, remove both secrets; authentication continues against the stored hash. There is no public reset endpoint. Account recovery needs the deployment owner's trusted database access.

Set `ALLOWED_ORIGINS` to a comma-separated list of the web frontend's exact HTTPS origins. The API's own origin is always allowed. Browser sessions use a Secure, HttpOnly, SameSite=Strict host cookie; cookie mutations require an allowed Origin. The native client uses `Authorization: Bearer TOKEN`; tokens in URL queries are never accepted. A non-Cloudflare adapter must provide a trusted client IP instead of accepting a caller-supplied `CF-Connecting-IP` header.

`DISCORD_USERNAME` defaults to `djdarknes.com_66953`. `DISCORD_CONTACT_URL` is optional; leave it empty until a real Discord URL is available. `OFFICIAL_REPOSITORIES` is a comma-separated allowlist of administrator-reviewed GitHub author repositories, merged with seven individually reviewed built-in defaults: `MDashK/Relic-Hunters-Zero-PS4`, `F1R3xS1NN3R/sound-of-nature`, `iHaiDeeZ/shattered-pixel-dungeon-ps4`, `Xyhlo/SSPI`, `thcolin/gamepad-media-center-aggregator`, `ScratchEverywhere/ScratchEverywhere` and `xfangfang/wiliwili`. All other known community hosts are tagged `community`, others `unknown`. These labels do not infer a license, piracy, package validity or firmware compatibility.

## API

All responses are JSON, `Cache-Control: no-store, no-transform`, and explicitly sized with `Content-Length`. The native client should request identity encoding. Public API timestamps are **Unix seconds**; internal SQLite timestamps use milliseconds. Errors have `{error,message}` and never serialize SQL, input passwords or exception details.

| Method/path | Access | Body/result |
| --- | --- | --- |
| GET `/api/public/config` | Public | Exact LivePix URL, plans, donation minimum, manual approval/Discord instructions |
| GET `/api/releases` | Public | `{releases:[{channel,version,url,size,sha256,updated_at}]}`; store binaries only |
| POST `/api/login` | Public, throttled | `{username,password}` → `{token,expires_at,user}`; also sets cookie |
| POST `/api/logout` | Logged in | Revokes the current session and clears cookie |
| GET `/api/session` | Logged in | `{user,expires_at}`; user includes `premium_active` |
| GET `/api/catalog` | Active premium/admin | `{version,updated_at,entries}` |
| GET `/api/admin/users` | Admin | Users' IDs/names/roles/plan/expiry/revocation; no secrets |
| POST `/api/admin/users` | Admin | `{username,password,plan}` → `{user}` |
| PATCH `/api/admin/users/:id` | Admin | Any of `{password,plan,revoked}` → `{user}` |
| POST `/api/admin/users/:id/revocation` | Admin | Exactly `{revoked:boolean}` → `{user}`; PS4-compatible alias |
| POST `/api/admin/users/:id/password` | Admin | Exactly `{password:string}` → `{user}`; PS4-compatible rotation alias |
| POST `/api/admin/catalog` | Admin | `{entries,expected_version?,replace?}` → `{version,updated_at,count}` |
| POST `/api/admin/releases` | Admin | `{channel:'normal'|'premium',version,url,size,sha256}` |

`user` is `{id,username,role,plan,expires_at,revoked,premium_active}`. Premium role expiration is checked against fresh database data on every catalog request, including already-issued sessions. Admin access is independent of premium expiration. Invalidating a premium account blocks its username/password and deletes every existing session in the same database transaction. Password rotation also deletes every session. Reactivation requires a new login: previous tokens never become valid again. The account-edit endpoints cannot edit an administrator, including the owner.

Session issuance conditionally checks the stored credential hash, salt and revocation status again after password verification; a login racing revocation or password rotation cannot issue a session with the old credentials. Account edits compare the original stored state before writing, returning `409 USER_CONFLICT` instead of overwriting a simultaneous account change. The update, session deletion and audit event share a transaction; a failed audit rolls back the full change. Server rejection takes effect when the invalidation operation returns. Connected clients should refresh `/api/session` every five seconds and block premium actions after 30 seconds without a successful check. This does not retract a direct public download URL already known to someone.

Sessions last eight hours. Failed, missing and revoked user logins produce the same response. Limits persist in D1: 10 attempts per username and 30 per trusted IP every ten minutes, including successful attempts; responses include `Retry-After` when limited.

Usernames accept 3–32 ASCII letters, digits, `_`, `.` or `-`, normalized to lowercase. Passwords accept 8–128 well-formed UTF-8 bytes without NUL, matching the console's C-string client. The administrator checks payment manually and creates or renews the account; there is no payment webhook or automatic payment claim. Plans are fixed: `15d` = **R$ 10 / 15 days**, `1m` = **R$ 20 / one calendar month**, `2m` = **R$ 30 / two calendar months**. Months use UTC calendar arithmetic and clamp to the target month's last day. Renewal extends the later of now/current expiry. Donations start at **R$ 1**. The exact payment URL is **https://livepix.gg/peppystore**. After payment the user sends proof to **djdarknes.com_66953** on Discord for manual approval.

## Catalog publishing

Publishing merges by ID unless `replace:true`; use `expected_version` for a reviewed edit. A transactional conditional insert and compare-and-swap prevents overwriting a concurrent edit. The latest three revisions remain in D1. Audit writes share the mutation transaction. Accounts are bounded to 5,000; remote catalogs to **1,024 entries and 1 MiB normalized JSON**, matching the native in-memory importer. Raw catalog request limit is 4 MiB; ordinary JSON bodies are limited to 16 KiB with streamed byte counting.

Entry fields:

```json
{
  "id": "example-homebrew",
  "name": "Example Homebrew",
  "version": "1.00",
  "size": 4096,
  "url": "https://author.example/releases/example.pkg",
  "filename": "example.pkg",
  "kind": "homebrew",
  "platform": "ps4",
  "content_id": "IV0000-BREW02048_00-GAME204800000000",
  "content_type": 26,
  "content_flags": 167772160,
  "iro_tag": 0,
  "is_theme": false,
  "language": ["pt-BR"],
  "requires_data": "",
  "description": "",
  "adult": false
}
```

This is a **schema example**, not a downloadable or verified package. `size` must be the exact PKG length, 1,080 bytes–256 GiB. CID/type/flags must come from package metadata, not a guessed game name. `source_kind` is recomputed by the service. Optional `sha256`, `cover_url` and `source_url` are preserved. An `adult` marker is metadata supplied by an administrator from a known rating; this service does not discover or fetch adult content.

Names are at most 128 UTF-8 bytes, filenames at most 95 ASCII bytes and end in lowercase `.pkg`; leading dots, traversal and duplicate local filenames are rejected. Version is empty for unknown or a package/release label at most 40 UTF-8 bytes; a release tag is not proof of the package's APP_VER. Description/data requirements are each at most 600 UTF-8 bytes. Display text rejects DEL/C1 controls, bidi overrides/isolates and unpaired surrogates, matching the console parser. CIDs are canonical 36-character PS4 IDs; the store's own `BREW00001` and PS5 `PPSA` IDs are rejected.

Kinds are `base`, `update`, `dlc`, `homebrew`, `media`, `theme`. Header classification follows the native importer: type `0x1E` = update; `0x1B/0x1C` = DLC unless flags mask `0x61300000` is set; type `0x1A` with that mask = update, otherwise only exact `0x0A000000/0x0E000000` = base. Homebrew/media require a base header. Themes require AC content type `0x1B`, a DLC header without patch flags, and IRO tag 1 or 2; `is_theme` must match. AL type `0x1C` stays ordinary DLC even with IRO tag 1 or 2. A SHAREfactory theme (IRO 1) receives a requirement note to have SHAREfactory installed when no requirement text was supplied. A base and its update may share a CID; their IDs/local filenames remain distinct.

URL validation requires HTTPS, a public dotted hostname, no credentials/fragments/nonstandard ports/backslashes, and a direct `.pkg` path (case-insensitive for remote assets). The sole landing-page exception is an exact public `mediafire.com` or `www.mediafire.com` URL shaped as `/file/<alphanumeric-key>/<filename.pkg>/file`, without a query. This matches the native downloader's existing bounded MediaFire resolver: raw filename characters are restricted, percent-encoded characters must decode to printable ASCII without slash/backslash, and `.rar`/HTML/other host or path shapes are rejected. This exception does not add a general blog resolver.

IP literals and local names are rejected. **The service does not fetch or resolve these URLs and has no PKG proxy endpoint.** It therefore cannot verify DNS, redirects, availability, bytes, a checksum, source ownership or compatibility merely from publication. Clients must independently reject private destinations/unsafe redirects, select only a provider-scoped download URL from the public landing HTML when necessary, and validate downloaded package headers, size and any pinned checksum before installing. Other website/RAR/password gates are not supported package links.

The tests execute this worker against the real SQL migration in an in-memory SQLite D1 adapter, including bootstrap races, password/session secrecy, role/expiry/revocation guards, plan rollover, catalog concurrency, header kinds, URL/body limits, CORS, public payment configuration and absence of remote fetches. They do not establish behavior on a real PS4 or a deployed Cloudflare account.

## Serviço publicado

Origem configurada no app: https://peppy-assets-updater.quick-chime-0602.chatgpt.site . O Peppy Assets Updater usa este backend com banco D1 persistente e a página em `web/index.html`. O catálogo inicial publicado vem de `seed-catalog.json`; consulte `native/premium-catalog-review.json` para a evidência. As credenciais de bootstrap são segredos de runtime e foram removidas após criar o administrador. Usuários novos são criados pelo administrador; pagar no LivePix não cria conta automaticamente.

O updater consulta a release pública fixa `akakakjajakaksk/ps4-store`, tag `v1.0.0`, para oferecer o PKG realmente publicado. Download pelo navegador não envia um payload nem instala um PKG automaticamente. O PKG dos serviços de streaming não inclui assinatura e requer verificação própria de funcionamento.

A página pública em `web/index.html` agora contém somente download da loja e instruções de instalação. Não apresenta login, catálogo premium ou administração. Essas operações são realizadas pelo cliente nativo do PS4 e usam a API autenticada deste serviço. O link direto do PKG permanece visível sem JavaScript.
