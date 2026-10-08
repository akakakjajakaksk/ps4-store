/** Portable Cloudflare Worker API. Secrets and account data never enter the PKG. */
export interface D1Statement {
  bind(...values: unknown[]): D1Statement;
  first<T = Record<string, unknown>>(): Promise<T | null>;
  all<T = Record<string, unknown>>(): Promise<{ results: T[] }>;
  run(): Promise<{ meta?: { changes?: number } }>;
}
export interface D1Database {
  prepare(sql: string): D1Statement;
  batch(statements: D1Statement[]): Promise<Array<{ meta?: { changes?: number } }>>;
}
export interface Env {
  DB: D1Database;
  ALLOWED_ORIGINS?: string;
  OWNER_USERNAME?: string;
  OWNER_PASSWORD?: string;
  DISCORD_CONTACT_URL?: string;
  DISCORD_USERNAME?: string;
  OFFICIAL_REPOSITORIES?: string;
}

type Role = 'admin' | 'premium';
type Plan = '15d' | '1m' | '2m';
type Kind = 'base' | 'update' | 'dlc' | 'homebrew' | 'media' | 'theme';
interface User {
  id: string; username: string; role: Role; password_salt: string;
  password_hash: string; password_iterations: number; plan: Plan | null;
  expires_at: number | null; revoked: number; created_at: number; updated_at: number;
}
interface Principal { user: User; sessionExpires: number; tokenHash: string; }
export interface CatalogEntry {
  id: string; name: string; version: string; size: number; url: string;
  filename: string; kind: Kind; source_kind: 'official' | 'community' | 'unknown';
  platform: 'ps4'; language: string[]; requires_data: string;
  content_id: string; content_type: number; content_flags: number; iro_tag: number;
  is_theme: boolean; adult: boolean; description: string;
  sha256?: string; cover_url?: string; source_url?: string;
}

const ITERATIONS = 100_000;
const SESSION_MS = 8 * 60 * 60 * 1000;
const LOGIN_WINDOW_MS = 10 * 60 * 1000;
const MAX_CATALOG_ITEMS = 1024;
const MAX_CATALOG_BYTES = 1024 * 1024;
const MAX_PKG_BYTES = 256 * 1024 * 1024 * 1024;
const COOKIE = '__Host-peppy_session';
const encoder = new TextEncoder();
const PLANS = [
  { id: '15d', price_brl: 10, duration_days: 15, label: '15 dias' },
  { id: '1m', price_brl: 20, duration_months: 1, label: '1 mês' },
  { id: '2m', price_brl: 30, duration_months: 2, label: '2 meses' },
] as const;

class ApiError extends Error {
  status: number;
  code: string;
  headers: Record<string, string>;
  constructor(status: number, code: string, message: string,
              headers: Record<string, string> = {}) {
    super(message); this.status = status; this.code = code; this.headers = headers;
  }
}
function fail(status: number, code: string, message: string): never {
  throw new ApiError(status, code, message);
}
function hex(bytes: Uint8Array): string {
  return Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
}
async function sha256(value: string): Promise<string> {
  return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', encoder.encode(value))));
}
function constantTimeHex(a: string, b: string): boolean {
  let difference = a.length ^ b.length;
  for (let i = 0; i < 64; i++) difference |= (a.charCodeAt(i) || 0) ^ (b.charCodeAt(i) || 0);
  return difference === 0;
}
function randomHex(bytes: number): string {
  return hex(crypto.getRandomValues(new Uint8Array(bytes)));
}
async function passwordHash(password: string, saltHex: string, iterations = ITERATIONS): Promise<string> {
  const salt = new Uint8Array(saltHex.match(/../g)!.map(value => parseInt(value, 16)));
  const key = await crypto.subtle.importKey('raw', encoder.encode(password), 'PBKDF2', false, ['deriveBits']);
  const bits = await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations }, key, 256);
  return hex(new Uint8Array(bits));
}
function username(value: unknown): string {
  if (typeof value !== 'string' || !/^[a-zA-Z0-9_.-]{3,32}$/.test(value))
    fail(400, 'INVALID_USERNAME', 'Use 3 a 32 letras, números, ponto, hífen ou sublinhado.');
  return value.toLowerCase();
}
function wellFormedUnicode(value: string): boolean {
  for (let index = 0; index < value.length; index++) {
    const unit = value.charCodeAt(index);
    if (unit >= 0xd800 && unit <= 0xdbff) {
      const next = value.charCodeAt(++index);
      if (!(next >= 0xdc00 && next <= 0xdfff)) return false;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) return false;
  }
  return true;
}
function password(value: unknown): string {
  if (typeof value !== 'string' || !wellFormedUnicode(value) || value.includes('\u0000') ||
      encoder.encode(value).length < 8 || encoder.encode(value).length > 128)
    fail(400, 'INVALID_PASSWORD', 'A senha deve conter de 8 a 128 bytes.');
  return value;
}
function textField(value: unknown, name: string, maximum: number, required = true): string {
  if (typeof value !== 'string' || !wellFormedUnicode(value) || value.length > maximum || (required && !value.trim()) ||
      /[\u0000-\u001f\u007f-\u009f\u202a-\u202e\u2066-\u2069]/.test(value))
    fail(400, 'INVALID_FIELD', `Campo inválido: ${name}.`);
  return value.trim();
}
function plan(value: unknown): Plan {
  if (value !== '15d' && value !== '1m' && value !== '2m') fail(400, 'INVALID_PLAN', 'Plano inválido.');
  return value;
}
/** Calendar months, clamped at the destination month's last day, always in UTC. */
export function planExpiration(selected: Plan, now: number): number {
  if (selected === '15d') return now + 15 * 24 * 60 * 60 * 1000;
  const date = new Date(now);
  const day = date.getUTCDate();
  date.setUTCDate(1);
  date.setUTCMonth(date.getUTCMonth() + (selected === '1m' ? 1 : 2));
  const lastDay = new Date(Date.UTC(date.getUTCFullYear(), date.getUTCMonth() + 1, 0)).getUTCDate();
  date.setUTCDate(Math.min(day, lastDay));
  return date.getTime();
}
function publicUser(user: User, now: number) {
  return { id: user.id, username: user.username, role: user.role, plan: user.plan,
    expires_at: user.expires_at === null ? null : Math.floor(user.expires_at / 1000),
    premium_active: user.role === 'admin' || (!user.revoked && user.expires_at !== null && user.expires_at > now) };
}
function allowedOrigins(env: Env, request: Request): Set<string> {
  const origins = new Set((env.ALLOWED_ORIGINS || '').split(',').map(value => value.trim()).filter(Boolean));
  origins.add(new URL(request.url).origin);
  return origins;
}
function cookieToken(request: Request): string | null {
  const raw = (request.headers.get('Cookie') || '').split(';').map(value => value.trim())
    .find(value => value.startsWith(`${COOKIE}=`));
  return raw ? raw.slice(COOKIE.length + 1) : null;
}
function requestToken(request: Request): string | null {
  const authorization = request.headers.get('Authorization');
  if (authorization !== null) return /^Bearer [a-f0-9]{64}$/.test(authorization) ? authorization.slice(7) : null;
  const token = cookieToken(request);
  return token && /^[a-f0-9]{64}$/.test(token) ? token : null;
}
function validateOrigin(request: Request, env: Env): void {
  const origin = request.headers.get('Origin');
  if (origin && !allowedOrigins(env, request).has(origin)) fail(403, 'ORIGIN_DENIED', 'Origem não permitida.');
  // Cookie-authenticated browser mutations require an Origin. Native bearer requests do not.
  if (!['GET', 'HEAD', 'OPTIONS'].includes(request.method) && cookieToken(request) &&
      !request.headers.has('Authorization') && !origin)
    fail(403, 'ORIGIN_REQUIRED', 'Origem obrigatória para esta sessão.');
}
async function jsonBody(request: Request, maximum = 16 * 1024): Promise<Record<string, unknown>> {
  if (!/^application\/json(?:\s*;|$)/i.test(request.headers.get('Content-Type') || ''))
    fail(415, 'JSON_REQUIRED', 'Envie JSON.');
  const length = request.headers.get('Content-Length');
  if (length !== null && (!/^\d+$/.test(length) || Number(length) > maximum))
    fail(413, 'BODY_TOO_LARGE', 'Requisição muito grande.');
  const reader = request.body?.getReader();
  if (!reader) fail(400, 'INVALID_JSON', 'JSON inválido.');
  const chunks: Uint8Array[] = [];
  let bytes = 0;
  while (true) {
    const part = await reader.read();
    if (part.done) break;
    bytes += part.value.byteLength;
    if (bytes > maximum) { await reader.cancel(); fail(413, 'BODY_TOO_LARGE', 'Requisição muito grande.'); }
    chunks.push(part.value);
  }
  const complete = new Uint8Array(bytes);
  let offset = 0;
  for (const chunk of chunks) { complete.set(chunk, offset); offset += chunk.length; }
  try {
    const result = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(complete));
    if (!result || Array.isArray(result) || typeof result !== 'object') throw new Error();
    return result;
  } catch { fail(400, 'INVALID_JSON', 'JSON inválido.'); }
}
/** Storage validation only. The service never resolves URLs or downloads remote bytes. */
export function publicHttpsUrl(value: unknown): string {
  const raw = textField(value, 'url', 2048);
  if (raw.includes('\\')) fail(400, 'INVALID_URL', 'URL inválida.');
  let url: URL;
  try { url = new URL(raw); } catch { fail(400, 'INVALID_URL', 'URL inválida.'); }
  const host = url.hostname.toLowerCase();
  // Reject all IP literals and local names; this service does not offer a fetch/proxy endpoint.
  if (url.protocol !== 'https:' || url.username || url.password || url.hash ||
      (url.port && url.port !== '443') || host.includes(':') || /^\d+(?:\.\d+){3}$/.test(host) ||
      !host.includes('.') || !/^[a-z0-9.-]+$/.test(host) ||
      /(?:^|\.)(?:localhost|local|internal|lan|home|test|invalid)$/.test(host))
    fail(400, 'INVALID_URL', 'Use uma URL HTTPS pública, sem credenciais ou endereço local.');
  return url.href;
}
function requireDirectPkg(url: string): void {
  let path: string;
  try { path = decodeURIComponent(new URL(url).pathname); }
  catch { fail(400, 'INVALID_URL', 'URL inválida.'); }
  if (!/\.pkg$/i.test(path)) fail(400, 'INDIRECT_URL', 'Informe o link direto de um arquivo PKG.');
}
function sourceKind(url: string, env: Env): CatalogEntry['source_kind'] {
  const parsed = new URL(url);
  if (parsed.hostname === 'github.com') {
    const parts = parsed.pathname.split('/').filter(Boolean);
    const repository = parts.slice(0, 2).join('/').toLowerCase();
    const official = new Set((env.OFFICIAL_REPOSITORIES || '').split(',').map(value => value.trim().toLowerCase()).filter(Boolean));
    return official.has(repository) ? 'official' : 'community';
  }
  const communityHosts = ['archive.org', 'mediafire.com', 'pkg-zone.com', 'superpsx.com', 'dlpsgame.com'];
  return communityHosts.some(host => parsed.hostname === host || parsed.hostname.endsWith(`.${host}`)) ? 'community' : 'unknown';
}
function normalizeEntry(raw: unknown, env: Env): CatalogEntry {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) fail(400, 'INVALID_ENTRY', 'Item inválido.');
  const entry = raw as Record<string, unknown>;
  const id = textField(entry.id, 'id', 80);
  if (!/^[a-zA-Z0-9_.-]+$/.test(id)) fail(400, 'INVALID_ENTRY', 'Identificador inválido.');
  const kind = entry.kind;
  if (!['base', 'update', 'dlc', 'homebrew', 'media', 'theme'].includes(String(kind)))
    fail(400, 'INVALID_KIND', 'Tipo de pacote inválido.');
  if (!Number.isSafeInteger(entry.size) || Number(entry.size) < 1080 || Number(entry.size) > MAX_PKG_BYTES)
    fail(400, 'INVALID_SIZE', 'Informe o tamanho exato do PKG, até 256 GiB.');
  const url = publicHttpsUrl(entry.url);
  const filename = textField(entry.filename, 'filename', 95);
  if (!/^[a-zA-Z0-9][a-zA-Z0-9_.-]*\.pkg$/.test(filename) || filename.includes('..'))
    fail(400, 'INVALID_FILENAME', 'Nome local inválido; use a extensão .pkg.');
  // MIME alone is not proof of a PKG. Client must verify bytes/header before installing.
  requireDirectPkg(url);
  if (entry.platform !== undefined && entry.platform !== 'ps4') fail(400, 'INVALID_PLATFORM', 'Somente PS4.');
  const language = entry.language === undefined ? [] : entry.language;
  if (!Array.isArray(language) || language.length > 12 || language.some(value => typeof value !== 'string' || !/^[a-zA-Z]{2,3}(?:-[a-zA-Z]{2,4})?$/.test(value)))
    fail(400, 'INVALID_LANGUAGE', 'Lista de idiomas inválida.');
  const contentId = textField(entry.content_id, 'content_id', 36);
  if (!/^[A-Z]{2}\d{4}-[A-Z]{4}\d{5}_\d{2}-[A-Z0-9]{16}$/.test(contentId) ||
      contentId.slice(7, 16) === 'BREW00001' || contentId.slice(7, 11) === 'PPSA')
    fail(400, 'INVALID_CONTENT_ID', 'Content ID inválido ou reservado.');
  const uint32 = (value: unknown, field: string): number => {
    if (!Number.isSafeInteger(value) || Number(value) < 0 || Number(value) > 0xffffffff)
      fail(400, 'INVALID_HEADER', `Campo de header inválido: ${field}.`);
    return Number(value);
  };
  const contentType = uint32(entry.content_type, 'content_type');
  const contentFlags = uint32(entry.content_flags, 'content_flags');
  const iroTag = entry.iro_tag === undefined ? 0 : uint32(entry.iro_tag, 'iro_tag');
  let headerKind: 'base' | 'update' | 'dlc';
  if (contentType === 0x1e) headerKind = 'update';
  else if (contentType === 0x1b || contentType === 0x1c) {
    if (contentFlags & 0x61300000) fail(400, 'INVALID_HEADER', 'Flags de DLC inválidas.');
    headerKind = 'dlc';
  } else if (contentType === 0x1a) {
    if (contentFlags & 0x61300000) headerKind = 'update';
    else if (contentFlags === 0x0a000000 || contentFlags === 0x0e000000) headerKind = 'base';
    else fail(400, 'INVALID_HEADER', 'Flags de base inválidas.');
  } else fail(400, 'INVALID_HEADER', 'Tipo de conteúdo PS4 inválido.');
  const isTheme = contentType === 0x1b && headerKind === 'dlc' && (iroTag === 1 || iroTag === 2);
  if (entry.is_theme !== undefined && (typeof entry.is_theme !== 'boolean' || entry.is_theme !== isTheme))
    fail(400, 'INVALID_HEADER', 'Tema deve corresponder ao IRO tag do pacote.');
  const expectedKind = kind === 'homebrew' || kind === 'media' ? 'base' : kind === 'theme' ? 'dlc' : kind;
  if (headerKind !== expectedKind || (kind === 'theme') !== isTheme)
    fail(400, 'INVALID_HEADER', 'Categoria diferente do header do pacote.');
  if (entry.adult !== undefined && typeof entry.adult !== 'boolean') fail(400, 'INVALID_FIELD', 'adult deve ser booleano.');
  const version = entry.version === undefined ? '' : textField(entry.version, 'version', 40, false);
  if (encoder.encode(version).length > 40) fail(400, 'INVALID_VERSION', 'Versão muito grande para o PS4.');
  const normalized: CatalogEntry = { id, name: textField(entry.name, 'name', 128),
    version, size: Number(entry.size), url, filename,
    kind: kind as Kind, source_kind: sourceKind(url, env), platform: 'ps4', language,
    requires_data: entry.requires_data === undefined ? '' : textField(entry.requires_data, 'requires_data', 600, false),
    description: entry.description === undefined ? '' : textField(entry.description, 'description', 600, false),
    content_id: contentId, content_type: contentType, content_flags: contentFlags, iro_tag: iroTag,
    is_theme: isTheme, adult: entry.adult === true };
  if (isTheme && iroTag === 1 && !normalized.requires_data)
    normalized.requires_data = 'Tema do SHAREfactory: exige o SHAREfactory instalado para usar o conteúdo.';
  if (encoder.encode(normalized.name).length > 128 || encoder.encode(normalized.requires_data).length > 600 ||
      encoder.encode(normalized.description).length > 600) fail(400, 'INVALID_FIELD', 'Texto muito grande para o PS4.');
  if (entry.sha256 !== undefined) {
    if (typeof entry.sha256 !== 'string' || !/^[a-fA-F0-9]{64}$/.test(entry.sha256)) fail(400, 'INVALID_SHA256', 'SHA-256 inválido.');
    normalized.sha256 = entry.sha256.toLowerCase();
  }
  if (entry.cover_url !== undefined) normalized.cover_url = publicHttpsUrl(entry.cover_url);
  if (entry.source_url !== undefined) normalized.source_url = publicHttpsUrl(entry.source_url);
  return normalized;
}
async function countAttempt(env: Env, key: string, maximum: number, now: number): Promise<void> {
  await env.DB.prepare(`INSERT INTO login_limits (key, window_start, attempts) VALUES (?, ?, 1)
    ON CONFLICT(key) DO UPDATE SET
      window_start = CASE WHEN window_start <= ? THEN excluded.window_start ELSE window_start END,
      attempts = CASE WHEN window_start <= ? THEN 1 ELSE attempts + 1 END`)
    .bind(key, now, now - LOGIN_WINDOW_MS, now - LOGIN_WINDOW_MS).run();
  const row = await env.DB.prepare('SELECT window_start, attempts FROM login_limits WHERE key = ?')
    .bind(key).first<{ window_start: number; attempts: number }>();
  if (!row || row.attempts > maximum) throw new ApiError(429, 'LOGIN_LIMIT', 'Tente novamente mais tarde.',
    { 'Retry-After': String(Math.max(1, Math.ceil(((row?.window_start || now) + LOGIN_WINDOW_MS - now) / 1000))) });
}
async function throttle(request: Request, env: Env, name: string, now: number): Promise<void> {
  // Cloudflare overwrites this header. A self-hosted adapter must supply its own trusted client IP.
  const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
  await countAttempt(env, `ip:${await sha256(ip)}`, 30, now);
  await countAttempt(env, `user:${await sha256(name)}`, 10, now);
  await env.DB.batch([
    env.DB.prepare('DELETE FROM login_limits WHERE window_start < ?').bind(now - 24 * 60 * 60 * 1000),
    env.DB.prepare('DELETE FROM sessions WHERE expires_at <= ?').bind(now),
  ]);
}
async function maybeBootstrap(env: Env, name: string, suppliedPassword: string, now: number): Promise<void> {
  if (!env.OWNER_USERNAME || !env.OWNER_PASSWORD) return;
  const admin = await env.DB.prepare("SELECT id FROM users WHERE role = 'admin' LIMIT 1").first();
  if (admin) return; // Existing, expired or revoked administrators are never overwritten.
  const matchesName = constantTimeHex(await sha256(name), await sha256(env.OWNER_USERNAME.toLowerCase()));
  const matchesPassword = constantTimeHex(await sha256(suppliedPassword), await sha256(env.OWNER_PASSWORD));
  if (!matchesName || !matchesPassword) return;
  username(env.OWNER_USERNAME);
  password(env.OWNER_PASSWORD);
  const salt = randomHex(16);
  const hash = await passwordHash(suppliedPassword, salt);
  await env.DB.prepare(`INSERT OR IGNORE INTO users
    (id, username, role, password_salt, password_hash, password_iterations, plan, expires_at, revoked, created_at, updated_at, approval_method)
    SELECT 'owner', ?, 'admin', ?, ?, ?, NULL, NULL, 0, ?, ?, 'runtime_owner_bootstrap'
    WHERE NOT EXISTS (SELECT 1 FROM users WHERE role = 'admin')`)
    .bind(name, salt, hash, ITERATIONS, now, now).run();
}
async function login(request: Request, env: Env, now: number) {
  const body = await jsonBody(request);
  const name = username(body.username);
  const suppliedPassword = password(body.password);
  await throttle(request, env, name, now);
  await maybeBootstrap(env, name, suppliedPassword, now);
  const user = await env.DB.prepare('SELECT * FROM users WHERE username = ?').bind(name).first<User>();
  const hash = await passwordHash(suppliedPassword, user?.password_salt || '00000000000000000000000000000000',
    user?.password_iterations || ITERATIONS);
  if (!user || !constantTimeHex(hash, user.password_hash) || user.revoked)
    fail(401, 'INVALID_LOGIN', 'Usuário ou senha inválidos.');
  const token = randomHex(32);
  const expires = now + SESSION_MS;
  await env.DB.prepare('INSERT INTO sessions (token_hash, user_id, expires_at, created_at) VALUES (?, ?, ?, ?)')
    .bind(await sha256(token), user.id, expires, now).run();
  return { body: { token, expires_at: Math.floor(expires / 1000), server_time: Math.floor(now / 1000), user: publicUser(user, now) }, headers: {
    'Set-Cookie': `${COOKIE}=${token}; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=${SESSION_MS / 1000}`,
  } };
}
async function principal(request: Request, env: Env, now: number): Promise<Principal> {
  const token = requestToken(request);
  if (!token) fail(401, 'LOGIN_REQUIRED', 'Entre na conta para continuar.');
  const tokenHash = await sha256(token);
  const row = await env.DB.prepare(`SELECT users.*, sessions.expires_at AS session_expires
    FROM sessions JOIN users ON users.id = sessions.user_id WHERE sessions.token_hash = ?`)
    .bind(tokenHash).first<User & { session_expires: number }>();
  if (!row || row.revoked || row.session_expires <= now) fail(401, 'SESSION_INVALID', 'Sessão inválida ou expirada.');
  return { user: row, sessionExpires: row.session_expires, tokenHash };
}
function requireAdmin(auth: Principal): void {
  if (auth.user.role !== 'admin') fail(403, 'ADMIN_REQUIRED', 'Acesso de administrador necessário.');
}
function requirePremium(auth: Principal, now: number): void {
  if (auth.user.role !== 'admin' && (auth.user.expires_at === null || auth.user.expires_at <= now))
    fail(403, 'PREMIUM_EXPIRED', 'Seu acesso premium expirou.');
}
async function catalog(env: Env) {
  const row = await env.DB.prepare(`SELECT catalog_state.version, catalog_state.updated_at, catalog_revisions.payload
    FROM catalog_state LEFT JOIN catalog_revisions ON catalog_revisions.version = catalog_state.version WHERE catalog_state.id = 1`)
    .first<{ version: number; updated_at: number; payload: string | null }>();
  return { version: row?.version || 0, updated_at: Math.floor((row?.updated_at || 0) / 1000), entries: row?.payload ? JSON.parse(row.payload) : [] };
}
async function saveCatalog(request: Request, env: Env, auth: Principal, now: number) {
  const body = await jsonBody(request, 4 * 1024 * 1024);
  if (!Array.isArray(body.entries) || body.entries.length > MAX_CATALOG_ITEMS)
    fail(400, 'CATALOG_LIMIT', 'Envie até 1024 itens por catálogo.');
  if (body.replace !== undefined && typeof body.replace !== 'boolean') fail(400, 'INVALID_REPLACE', 'replace deve ser booleano.');
  const incoming = body.entries.map(entry => normalizeEntry(entry, env));
  const seen = new Set<string>();
  for (const entry of incoming) {
    if (seen.has(entry.id)) fail(400, 'DUPLICATE_ID', 'Identificador duplicado no catálogo.');
    seen.add(entry.id);
  }
  // Managed schema migrations contain no seed SQL; initialize the singleton lazily.
  await env.DB.prepare('INSERT OR IGNORE INTO catalog_state (id, version, updated_at) VALUES (1, 0, 0)').run();
  const previous = await catalog(env);
  const expected = body.expected_version === undefined ? previous.version : body.expected_version;
  if (!Number.isSafeInteger(expected) || Number(expected) < 0) fail(400, 'INVALID_VERSION', 'Versão inválida.');
  if (expected !== previous.version) fail(409, 'CATALOG_CONFLICT', 'O catálogo mudou; atualize antes de salvar.');
  const entries = body.replace === true ? incoming : Array.from(new Map<string, CatalogEntry>(
    [...previous.entries, ...incoming].map((entry: CatalogEntry) => [entry.id, entry])).values());
  if (entries.length > MAX_CATALOG_ITEMS) fail(400, 'CATALOG_LIMIT', 'O catálogo aceita até 1024 itens.');
  const filenames = new Set<string>();
  for (const entry of entries) {
    if (filenames.has(entry.filename)) fail(400, 'DUPLICATE_FILENAME', 'Nome local de PKG duplicado.');
    filenames.add(entry.filename);
  }
  const payload = JSON.stringify(entries);
  if (encoder.encode(payload).length > MAX_CATALOG_BYTES) fail(413, 'CATALOG_TOO_LARGE', 'O catálogo normalizado excedeu 1 MiB.');
  const next = previous.version + 1;
  // D1 batch is transactional. Conditional insertion+CAS prevents a stale writer from replacing data.
  const results = await env.DB.batch([
    env.DB.prepare(`INSERT INTO catalog_revisions (version, payload, updated_at, actor_id)
      SELECT ?, ?, ?, ? WHERE (SELECT version FROM catalog_state WHERE id = 1) = ?`)
      .bind(next, payload, now, auth.user.id, expected),
    env.DB.prepare(`UPDATE catalog_state SET version = ?, updated_at = ? WHERE id = 1 AND version = ?
      AND EXISTS (SELECT 1 FROM catalog_revisions WHERE version = ?)`)
      .bind(next, now, expected, next),
    env.DB.prepare(`INSERT INTO audit_events (actor_id, action, entity_id, created_at)
      SELECT ?, 'catalog.publish', ?, ? WHERE (SELECT version FROM catalog_state WHERE id = 1) = ?
      AND changes() = 1`).bind(auth.user.id, String(next), now, next),
    env.DB.prepare('DELETE FROM catalog_revisions WHERE version < (SELECT version - 2 FROM catalog_state WHERE id = 1)'),
  ]);
  if (results[1]?.meta?.changes !== 1) fail(409, 'CATALOG_CONFLICT', 'O catálogo mudou; atualize antes de salvar.');
  return { version: next, updated_at: Math.floor(now / 1000), count: entries.length };
}
async function createUser(request: Request, env: Env, auth: Principal, now: number) {
  const body = await jsonBody(request);
  const name = username(body.username);
  const suppliedPassword = password(body.password);
  const selected = plan(body.plan);
  if (await env.DB.prepare('SELECT id FROM users WHERE username = ?').bind(name).first())
    fail(409, 'USER_EXISTS', 'Usuário já existe.');
  const count = await env.DB.prepare('SELECT COUNT(*) AS count FROM users').first<{ count: number }>();
  if ((count?.count || 0) >= 5000) fail(409, 'USER_LIMIT', 'Limite de usuários atingido.');
  const id = randomHex(16);
  const salt = randomHex(16);
  const hash = await passwordHash(suppliedPassword, salt);
  const expires = planExpiration(selected, now);
  const results = await env.DB.batch([env.DB.prepare(`INSERT OR IGNORE INTO users
    (id, username, role, password_salt, password_hash, password_iterations, plan, expires_at, revoked, created_at, updated_at, approved_by)
    SELECT ?, ?, 'premium', ?, ?, ?, ?, ?, 0, ?, ?, ? WHERE (SELECT COUNT(*) FROM users) < 5000`)
    .bind(id, name, salt, hash, ITERATIONS, selected, expires, now, now, auth.user.id),
    env.DB.prepare(`INSERT INTO audit_events (actor_id, action, entity_id, created_at)
      SELECT ?, 'user.create', ?, ? WHERE changes() = 1`).bind(auth.user.id, id, now)]);
  if (results[0]?.meta?.changes !== 1) fail(409, 'USER_EXISTS', 'Usuário já existe ou limite atingido.');
  return { user: { id, username: name, role: 'premium', plan: selected, expires_at: Math.floor(expires / 1000), premium_active: true } };
}
async function editUser(request: Request, env: Env, auth: Principal, id: string, now: number) {
  const user = await env.DB.prepare('SELECT * FROM users WHERE id = ?').bind(id).first<User>();
  if (!user || user.role !== 'premium') fail(404, 'USER_NOT_FOUND', 'Usuário premium não encontrado.');
  const body = await jsonBody(request);
  const keys = Object.keys(body);
  if (!keys.length || keys.some(key => !['password', 'plan', 'revoked'].includes(key)))
    fail(400, 'INVALID_UPDATE', 'Informe senha, plano ou revogação.');
  if (body.revoked !== undefined && typeof body.revoked !== 'boolean') fail(400, 'INVALID_UPDATE', 'revoked deve ser booleano.');
  let salt = user.password_salt;
  let hash = user.password_hash;
  if (body.password !== undefined) { salt = randomHex(16); hash = await passwordHash(password(body.password), salt); }
  const selected = body.plan === undefined ? user.plan : plan(body.plan);
  const expires = body.plan === undefined ? user.expires_at : planExpiration(selected!, Math.max(now, user.expires_at || 0));
  const revoked = body.revoked === undefined ? user.revoked : Number(body.revoked);
  await env.DB.batch([
    env.DB.prepare(`UPDATE users SET password_salt = ?, password_hash = ?, password_iterations = ?,
      plan = ?, expires_at = ?, revoked = ?, updated_at = ?, approved_by = ? WHERE id = ? AND role = 'premium'`)
      .bind(salt, hash, ITERATIONS, selected, expires, revoked, now, auth.user.id, id),
    // Rotation/revocation invalidates every existing session immediately.
    env.DB.prepare('DELETE FROM sessions WHERE user_id = ? AND ? = 1')
      .bind(id, Number(body.password !== undefined || body.revoked !== undefined)),
    env.DB.prepare('INSERT INTO audit_events (actor_id, action, entity_id, created_at) VALUES (?, ?, ?, ?)')
      .bind(auth.user.id, 'user.update', id, now),
  ]);
  return { user: publicUser({ ...user, password_salt: salt, password_hash: hash, plan: selected, expires_at: expires, revoked }, now) };
}
async function saveRelease(request: Request, env: Env, auth: Principal, now: number) {
  const body = await jsonBody(request);
  if (body.channel !== 'normal' && body.channel !== 'premium') fail(400, 'INVALID_CHANNEL', 'Canal inválido.');
  const url = publicHttpsUrl(body.url);
  requireDirectPkg(url);
  if (!Number.isSafeInteger(body.size) || Number(body.size) < 1080 || Number(body.size) > MAX_PKG_BYTES)
    fail(400, 'INVALID_SIZE', 'Tamanho do PKG inválido.');
  if (typeof body.sha256 !== 'string' || !/^[a-fA-F0-9]{64}$/.test(body.sha256)) fail(400, 'INVALID_SHA256', 'SHA-256 obrigatório.');
  const payload = { channel: body.channel, version: textField(body.version, 'version', 40), url,
    size: Number(body.size), sha256: body.sha256.toLowerCase(), updated_at: Math.floor(now / 1000) };
  await env.DB.batch([env.DB.prepare(`INSERT INTO releases (channel, payload, updated_at, actor_id) VALUES (?, ?, ?, ?)
    ON CONFLICT(channel) DO UPDATE SET payload = excluded.payload, updated_at = excluded.updated_at, actor_id = excluded.actor_id`)
    .bind(body.channel, JSON.stringify(payload), now, auth.user.id),
    env.DB.prepare('INSERT INTO audit_events (actor_id, action, entity_id, created_at) VALUES (?, ?, ?, ?)')
      .bind(auth.user.id, 'release.publish', body.channel, now)]);
  return payload;
}

async function route(request: Request, env: Env, now: number) {
  const path = new URL(request.url).pathname;
  const method = request.method;
  if (method === 'GET' && path === '/api/public/config') {
    let contact: string | null = null;
    if (env.DISCORD_CONTACT_URL) contact = publicHttpsUrl(env.DISCORD_CONTACT_URL);
    return { livepix_url: 'https://livepix.gg/peppystore', donation_minimum_brl: 1,
      plans: PLANS, approval: 'manual_admin', discord_contact_url: contact,
      discord_username: env.DISCORD_USERNAME || 'djdarknes.com_66953',
      payment_instructions: 'Doações: a partir de R$ 1. Premium: pague exatamente o valor do plano. Após pagar, envie o comprovante no Discord para djdarknes.com_66953; o acesso é liberado após conferência manual.',
      source_classification: 'Origem informativa; não determina licença, legalidade ou compatibilidade.' };
  }
  if (!env.DB) fail(503, 'HUB_UNAVAILABLE', 'Serviço ainda não configurado.');
  if (method === 'POST' && path === '/api/login') return login(request, env, now);
  if (method === 'GET' && path === '/api/releases') {
    const rows = await env.DB.prepare('SELECT payload FROM releases ORDER BY channel').all<{ payload: string }>();
    // Releases contain store binaries, never game catalogs or users.
    return { releases: rows.results.map(row => JSON.parse(row.payload)) };
  }
  if (!path.startsWith('/api/')) fail(404, 'NOT_FOUND', 'Rota não encontrada.');
  const auth = await principal(request, env, now);
  if (method === 'POST' && path === '/api/logout') {
    await env.DB.prepare('DELETE FROM sessions WHERE token_hash = ?').bind(auth.tokenHash).run();
    return { body: { ok: true }, headers: { 'Set-Cookie': `${COOKIE}=; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=0` } };
  }
  if (method === 'GET' && path === '/api/session')
    return { user: publicUser(auth.user, now), expires_at: Math.floor(auth.sessionExpires / 1000), server_time: Math.floor(now / 1000) };
  if (method === 'GET' && path === '/api/catalog') { requirePremium(auth, now); return catalog(env); }
  if (path.startsWith('/api/admin/')) {
    requireAdmin(auth);
    if (method === 'GET' && path === '/api/admin/users') {
      const users = await env.DB.prepare('SELECT id, username, role, plan, expires_at, revoked, created_at, updated_at FROM users ORDER BY created_at DESC LIMIT 5000')
        .all<User>();
      return { users: users.results.map(user => ({ ...publicUser(user, now), revoked: Boolean(user.revoked), created_at: Math.floor(user.created_at / 1000) })) };
    }
    if (method === 'POST' && path === '/api/admin/users') return createUser(request, env, auth, now);
    const match = path.match(/^\/api\/admin\/users\/([a-f0-9]{32})$/);
    if (method === 'PATCH' && match) return editUser(request, env, auth, match[1], now);
    if (method === 'POST' && path === '/api/admin/catalog') return saveCatalog(request, env, auth, now);
    if (method === 'POST' && path === '/api/admin/releases') return saveRelease(request, env, auth, now);
  }
  fail(404, 'NOT_FOUND', 'Rota não encontrada.');
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    let payload: unknown;
    let status = 200;
    let additional: Record<string, string> = {};
    try {
      validateOrigin(request, env);
      if (request.method === 'OPTIONS') {
        return new Response(null, { status: 204, headers: responseHeaders(request, env, {}) });
      }
      const result = await route(request, env, Date.now());
      if (result && typeof result === 'object' && 'body' in result && 'headers' in result) {
        payload = result.body; additional = result.headers;
      } else payload = result;
    } catch (error) {
      if (error instanceof ApiError) {
        status = error.status; payload = { error: error.code, message: error.message }; additional = error.headers;
      } else {
        // Never serialize exception text, SQL parameters, request bodies, or runtime secrets.
        status = 500; payload = { error: 'INTERNAL_ERROR', message: 'Não foi possível concluir a operação.' };
      }
    }
    const encoded = JSON.stringify(payload);
    additional['Content-Length'] = String(encoder.encode(encoded).length);
    return new Response(encoded, { status, headers: responseHeaders(request, env, additional) });
  },
};
function responseHeaders(request: Request, env: Env, additional: Record<string, string>): Headers {
  const headers = new Headers({ 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store, no-transform',
    'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', ...additional });
  const origin = request.headers.get('Origin');
  if (origin && allowedOrigins(env, request).has(origin)) {
    headers.set('Access-Control-Allow-Origin', origin);
    headers.set('Access-Control-Allow-Credentials', 'true');
    headers.set('Access-Control-Allow-Headers', 'Authorization, Content-Type');
    headers.set('Access-Control-Allow-Methods', 'GET, POST, PATCH, OPTIONS');
    headers.set('Vary', 'Origin');
  }
  return headers;
}
