import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { readFileSync } from 'node:fs';
import worker, { planExpiration } from '../src/worker.ts';

// Execute the real migration and worker queries against SQLite, with D1-shaped bindings.
class SqliteD1 {
  constructor() {
    this.sqlite = new DatabaseSync(':memory:');
    this.sqlite.exec(readFileSync(new URL('../migrations/0001_hub.sql', import.meta.url), 'utf8'));
  }
  prepare(sql) { return new Statement(this, sql); }
  async batch(statements) {
    this.sqlite.exec('BEGIN');
    try {
      const results = statements.map(statement => statement.runSync());
      this.sqlite.exec('COMMIT');
      return results;
    } catch (error) { this.sqlite.exec('ROLLBACK'); throw error; }
  }
}
class Statement {
  constructor(db, sql, bindings = []) { this.db = db; this.sql = sql; this.bindings = bindings; }
  bind(...bindings) { return new Statement(this.db, this.sql, bindings); }
  async first() { return this.db.sqlite.prepare(this.sql).get(...this.bindings) || null; }
  async all() { return { results: this.db.sqlite.prepare(this.sql).all(...this.bindings) }; }
  runSync() { const result = this.db.sqlite.prepare(this.sql).run(...this.bindings); return { meta: { changes: result.changes } }; }
  async run() { return this.runSync(); }
}
const clock = Date.UTC(2026, 0, 31, 13, 40, 10);
Date.now = () => clock;
const endpoint = 'https://hub.peppy.example';
function fixture() {
  const ownerPassword = `test-only-${crypto.randomUUID()}`;
  const env = { DB: new SqliteD1(), ALLOWED_ORIGINS: 'https://peppy.example',
    OWNER_USERNAME: 'test_owner', OWNER_PASSWORD: ownerPassword,
    OFFICIAL_REPOSITORIES: 'test-author/test-game' };
  const request = async (path, { method = 'GET', body, token, headers = {} } = {}) => {
    const options = { method, headers: { 'CF-Connecting-IP': '198.51.100.7', ...headers } };
    if (body !== undefined) { options.body = JSON.stringify(body); options.headers['Content-Type'] = 'application/json'; }
    if (token) options.headers.Authorization = `Bearer ${token}`;
    const response = await worker.fetch(new Request(`${endpoint}${path}`, options), env);
    const json = response.status === 204 ? null : await response.json();
    return { response, status: response.status, json };
  };
  const owner = async () => {
    const login = await request('/api/login', { method: 'POST', body: { username: env.OWNER_USERNAME, password: ownerPassword } });
    assert.equal(login.status, 200);
    return login;
  };
  const member = async (adminToken, name = 'test_member', selected = '15d') => {
    const secret = `test-only-${crypto.randomUUID()}`;
    const created = await request('/api/admin/users', { method: 'POST', token: adminToken,
      body: { username: name, password: secret, plan: selected } });
    assert.equal(created.status, 200);
    const login = await request('/api/login', { method: 'POST', body: { username: name, password: secret } });
    assert.equal(login.status, 200);
    return { ...login, id: created.json.user.id, secret };
  };
  return { env, request, owner, member, ownerPassword };
}
function entry(overrides = {}) {
  return { id: 'test-game', name: 'Test Game', version: '1.00', size: 4096,
    url: 'https://github.com/test-author/test-game/releases/download/v1/test.pkg', filename: 'test.pkg',
    kind: 'base', content_id: 'IV0000-BREW02048_00-GAME204800000000', content_type: 0x1a,
    content_flags: 0x0a000000, language: ['pt-BR'], ...overrides };
}
function assertNoSecrets(value) {
  const serialized = JSON.stringify(value);
  for (const field of ['password_hash', 'password_salt', 'password_iterations', 'token_hash', 'OWNER_PASSWORD'])
    assert.equal(serialized.includes(field), false, field);
}

test('public config has exact LivePix, prices, Discord and manual approval; no account data', async () => {
  const { request } = fixture();
  const result = await request('/api/public/config');
  assert.equal(result.status, 200);
  assert.equal(result.json.livepix_url, 'https://livepix.gg/peppystore');
  assert.equal(result.json.discord_username, 'djdarknes.com_66953');
  assert.equal(result.json.donation_minimum_brl, 1);
  assert.deepEqual(result.json.plans.map(plan => [plan.id, plan.price_brl]), [['15d', 10], ['1m', 20], ['2m', 30]]);
  assert.equal(result.json.approval, 'manual_admin');
  assert.equal(result.json.discord_contact_url, null);
  assert.equal('users' in result.json, false);
  assertNoSecrets(result.json);
});

test('owner bootstrap is once-only, salted PBKDF2; session storage never contains bearer token', async () => {
  const { env, owner, request, ownerPassword } = fixture();
  const login = await owner();
  const stored = env.DB.sqlite.prepare('SELECT * FROM users').get();
  assert.equal(stored.id, 'owner');
  assert.equal(stored.role, 'admin');
  assert.equal(stored.password_iterations, 100000);
  assert.match(stored.password_salt, /^[a-f0-9]{32}$/);
  assert.match(stored.password_hash, /^[a-f0-9]{64}$/);
  assert.notEqual(stored.password_hash, ownerPassword);
  const session = env.DB.sqlite.prepare('SELECT * FROM sessions').get();
  assert.match(session.token_hash, /^[a-f0-9]{64}$/);
  assert.notEqual(session.token_hash, login.json.token);
  assert.equal(login.json.expires_at, (clock + 8 * 60 * 60 * 1000) / 1000);
  assert.equal(login.json.server_time, clock / 1000);
  const currentSession = await request('/api/session', { token: login.json.token });
  assert.equal(currentSession.status, 200);
  assert.equal(currentSession.json.server_time, clock / 1000);
  assert.equal(currentSession.json.expires_at - currentSession.json.server_time, 8 * 60 * 60);
  assert.match(login.response.headers.get('Set-Cookie'), /HttpOnly; Secure; SameSite=Strict/);
  assert.equal(login.response.headers.get('Cache-Control'), 'no-store, no-transform');
  assert.equal(Number(login.response.headers.get('Content-Length')), new TextEncoder().encode(JSON.stringify(login.json)).length);
  assertNoSecrets(login.json);
  env.OWNER_PASSWORD = `test-only-${crypto.randomUUID()}`;
  const wrong = await request('/api/login', { method: 'POST', body: { username: env.OWNER_USERNAME, password: env.OWNER_PASSWORD } });
  assert.equal(wrong.status, 401);
  const original = await request('/api/login', { method: 'POST', body: { username: env.OWNER_USERNAME, password: ownerPassword } });
  assert.equal(original.status, 200);
  assert.equal(env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM users').get().n, 1);
});

test('concurrent initial owner logins claim one admin; missing bootstrap secrets do not initialize', async () => {
  const { env, owner } = fixture();
  await Promise.all([owner(), owner()]);
  assert.equal(env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM users').get().n, 1);
  const empty = fixture();
  delete empty.env.OWNER_PASSWORD;
  const result = await empty.request('/api/login', { method: 'POST', body: { username: 'test_owner', password: empty.ownerPassword } });
  assert.equal(result.status, 401);
  assert.equal(empty.env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM users').get().n, 0);
});

test('premium catalog and admin writes are guarded; expiration/revocation checked after login', async () => {
  const { env, request, owner, member } = fixture();
  assert.equal((await request('/api/catalog')).status, 401);
  const admin = await owner();
  const premium = await member(admin.json.token);
  assert.equal(premium.json.user.premium_active, true);
  assert.equal((await request('/api/admin/users', { token: premium.json.token })).status, 403);
  assert.equal((await request('/api/admin/catalog', { method: 'POST', token: premium.json.token, body: { entries: [] } })).status, 403);
  assert.equal((await request('/api/catalog', { token: premium.json.token })).status, 200);
  env.DB.sqlite.prepare('UPDATE users SET expires_at = ? WHERE id = ?').run(clock, premium.id);
  const expired = await request('/api/catalog', { token: premium.json.token });
  assert.equal(expired.status, 403);
  assert.equal(expired.json.error, 'PREMIUM_EXPIRED');
  assert.equal((await request('/api/session', { token: premium.json.token })).json.user.premium_active, false);
  const revoked = await request(`/api/admin/users/${premium.id}`, { method: 'PATCH', token: admin.json.token, body: { revoked: true } });
  assert.equal(revoked.status, 200);
  assert.equal(env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?').get(premium.id).n, 0);
  assert.equal((await request('/api/session', { token: premium.json.token })).status, 401);
  assert.equal((await request('/api/login', { method: 'POST', body: { username: 'test_member', password: premium.secret } })).status, 401);
  assert.equal((await request('/api/catalog', { token: admin.json.token })).status, 200);
});

test('calendar plans, renewals, password rotation and manual account metadata', async () => {
  assert.equal(planExpiration('15d', clock), Date.UTC(2026, 1, 15, 13, 40, 10));
  assert.equal(planExpiration('1m', clock), Date.UTC(2026, 1, 28, 13, 40, 10));
  assert.equal(planExpiration('2m', clock), Date.UTC(2026, 2, 31, 13, 40, 10));
  const { env, request, owner, member } = fixture();
  const admin = await owner();
  const premium = await member(admin.json.token, 'monthly_user', '1m');
  const row = env.DB.sqlite.prepare('SELECT * FROM users WHERE id = ?').get(premium.id);
  assert.equal(row.approval_method, 'manual_admin');
  assert.equal(row.approved_by, 'owner');
  const updated = await request(`/api/admin/users/${premium.id}`, { method: 'PATCH', token: admin.json.token, body: { plan: '1m' } });
  assert.equal(updated.json.user.expires_at, Date.UTC(2026, 2, 28, 13, 40, 10) / 1000);
  const newSecret = `test-only-${crypto.randomUUID()}`;
  assert.equal((await request(`/api/admin/users/${premium.id}`, { method: 'PATCH', token: admin.json.token, body: { password: newSecret } })).status, 200);
  assert.equal((await request('/api/session', { token: premium.json.token })).status, 401);
  assert.equal((await request('/api/login', { method: 'POST', body: { username: 'monthly_user', password: premium.secret } })).status, 401);
  assert.equal((await request('/api/login', { method: 'POST', body: { username: 'monthly_user', password: newSecret } })).status, 200);
  const listing = await request('/api/admin/users', { token: admin.json.token });
  assert.equal(listing.status, 200);
  assertNoSecrets(listing.json);
  assert.equal(listing.json.users.length, 2);
});

test('created premium account is immediately listed and usable by an independent client without a release update', async () => {
  const { env, request, owner } = fixture();
  const admin = await owner();
  const published = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
    body: { entries: [entry()], expected_version: 0 } });
  assert.equal(published.status, 200);

  const secret = `test-only-${crypto.randomUUID()}`;
  const credentials = { username: 'new_live_member', password: secret, plan: '15d' };
  const created = await request('/api/admin/users', { method: 'POST', token: admin.json.token, body: credentials });
  assert.equal(created.status, 200);
  assertNoSecrets(created.json);
  assert.equal(JSON.stringify(created.json).includes(secret), false);

  // The second device sees only committed D1 data, with no owner bootstrap secret,
  // inherited bearer token, cookies, client list, or release published for this account.
  const independentEnv = { DB: env.DB, ALLOWED_ORIGINS: env.ALLOWED_ORIGINS,
    OFFICIAL_REPOSITORIES: env.OFFICIAL_REPOSITORIES };
  const independent = async (path, options = {}) => {
    const response = await worker.fetch(new Request(`${endpoint}${path}`, options), independentEnv);
    return { response, status: response.status, json: await response.json() };
  };
  const listing = await independent('/api/admin/users', { headers: { Authorization: `Bearer ${admin.json.token}` } });
  assert.equal(listing.status, 200);
  // All fixtures intentionally use the same timestamp: do not assume the new
  // account precedes the owner when selecting which row may be invalidated.
  const listed = listing.json.users.find(user => user.id === created.json.user.id);
  assert.deepEqual(listed, { ...created.json.user, created_at: clock / 1000 });
  assert.equal(listing.json.users.some(user => user.role === 'admin'), true);
  assert.equal(listing.response.headers.get('Cache-Control'), 'no-store, no-transform');
  assertNoSecrets(listing.json);
  assert.equal(JSON.stringify(listing.json).includes(secret), false);

  const login = await independent('/api/login', { method: 'POST',
    headers: { 'Content-Type': 'application/json', 'CF-Connecting-IP': '203.0.113.44' },
    body: JSON.stringify({ username: credentials.username, password: secret }) });
  assert.equal(login.status, 200);
  assert.equal(login.json.user.id, created.json.user.id);
  assert.equal(login.json.user.premium_active, true);
  const catalog = await independent('/api/catalog', { headers: { Authorization: `Bearer ${login.json.token}` } });
  assert.equal(catalog.status, 200);
  assert.equal(catalog.json.version, published.json.version);
  assert.deepEqual(catalog.json.entries.map(item => item.id), ['test-game']);
  assert.deepEqual((await independent('/api/releases')).json, { releases: [] });

  const duplicate = await request('/api/admin/users', { method: 'POST', token: admin.json.token,
    body: { ...credentials, username: credentials.username.toUpperCase(), password: `test-only-${crypto.randomUUID()}` } });
  assert.equal(duplicate.status, 409);
  assert.equal(duplicate.json.error, 'USER_EXISTS');
  const stored = env.DB.sqlite.prepare('SELECT * FROM users WHERE id = ?').get(created.json.user.id);
  assert.notEqual(stored.password_hash, secret);
  assert.match(stored.password_salt, /^[a-f0-9]{32}$/);
  assert.equal(env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM users').get().n, 2);
  assert.equal(env.DB.sqlite.prepare("SELECT COUNT(*) AS n FROM audit_events WHERE action = 'user.create'").get().n, 1);
  assert.equal((await independent('/api/session', { headers: { Authorization: `Bearer ${login.json.token}` } })).status, 200);
});

test('invalidating an account destroys every session; reactivation never revives tokens', async () => {
  const { env, request, owner, member } = fixture();
  const admin = await owner();
  const premium = await member(admin.json.token, 'invalidated_user');
  const unaffected = await member(admin.json.token, 'unaffected_user');
  const second = await request('/api/login', { method: 'POST', body: { username: 'invalidated_user', password: premium.secret } });
  assert.equal(second.status, 200);
  const tokens = [premium.json.token, second.json.token];
  const response = await request(`/api/admin/users/${premium.id}/revocation`, {
    method: 'POST', token: admin.json.token, body: { revoked: true } });
  assert.equal(response.status, 200);
  assert.equal(response.json.user.revoked, true);
  assert.equal(response.json.user.premium_active, false);
  assert.equal(env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?').get(premium.id).n, 0);
  for (const token of tokens) {
    assert.equal((await request('/api/session', { token })).status, 401);
    assert.equal((await request('/api/catalog', { token })).status, 401);
    assert.equal((await request('/api/logout', { method: 'POST', token })).status, 401);
  }
  assert.equal((await request('/api/login', { method: 'POST', body: { username: 'invalidated_user', password: premium.secret } })).status, 401);
  const invalidatedListing = await request('/api/admin/users', { token: admin.json.token });
  assert.equal(invalidatedListing.json.users.find(user => user.id === premium.id).revoked, true);
  assert.equal(invalidatedListing.json.users.find(user => user.id === premium.id).premium_active, false);
  assert.equal((await request('/api/session', { token: unaffected.json.token })).status, 200);
  assert.equal((await request('/api/catalog', { token: unaffected.json.token })).status, 200);
  const reactivated = await request(`/api/admin/users/${premium.id}/revocation`, {
    method: 'POST', token: admin.json.token, body: { revoked: false } });
  assert.equal(reactivated.status, 200);
  assert.equal(reactivated.json.user.revoked, false);
  const reactivatedListing = await request('/api/admin/users', { token: admin.json.token });
  assert.equal(reactivatedListing.json.users.find(user => user.id === premium.id).premium_active, true);
  for (const token of tokens) assert.equal((await request('/api/session', { token })).status, 401);
  const fresh = await request('/api/login', { method: 'POST', body: { username: 'invalidated_user', password: premium.secret } });
  assert.equal(fresh.status, 200);
  assert.equal((await request('/api/catalog', { token: fresh.json.token })).status, 200);
  assert.equal(env.DB.sqlite.prepare("SELECT COUNT(*) AS n FROM audit_events WHERE action = 'user.invalidate'").get().n, 1);
  assert.equal(env.DB.sqlite.prepare("SELECT COUNT(*) AS n FROM audit_events WHERE action = 'user.reactivate'").get().n, 1);
});

test('revocation alias is admin-only, validates exact input, and cannot edit the owner', async () => {
  const { request, owner, member } = fixture();
  const admin = await owner();
  const premium = await member(admin.json.token);
  const path = `/api/admin/users/${premium.id}/revocation`;
  assert.equal((await request(path, { method: 'POST', body: { revoked: true } })).status, 401);
  assert.equal((await request(path, { method: 'POST', token: premium.json.token, body: { revoked: true } })).status, 403);
  for (const body of [{}, { revoked: 'true' }, { revoked: true, password: 'test-password' }, { plan: '15d' }])
    assert.equal((await request(path, { method: 'POST', token: admin.json.token, body })).status, 400);
  assert.equal((await request('/api/admin/users/owner/revocation', { method: 'POST', token: admin.json.token, body: { revoked: true } })).status, 404);
  assert.equal((await request('/api/admin/users/owner', { method: 'PATCH', token: admin.json.token, body: { password: 'test-password', revoked: true } })).status, 404);
  assert.equal((await request('/api/session', { token: admin.json.token })).status, 200);
});

test('native password alias rotates credentials, destroys all sessions and restricts input/role', async () => {
  const { env, request, owner, member } = fixture();
  const admin = await owner();
  const premium = await member(admin.json.token, 'rotated_user');
  const path = `/api/admin/users/${premium.id}/password`;
  assert.equal((await request(path, { method: 'POST', body: { password: 'test-only-password' } })).status, 401);
  assert.equal((await request(path, { method: 'POST', token: premium.json.token, body: { password: 'test-only-password' } })).status, 403);
  for (const body of [{}, { password: 123456789 }, { password: 'short' }, { password: 'test-only-password', revoked: false }, { plan: '1m' }])
    assert.equal((await request(path, { method: 'POST', token: admin.json.token, body })).status, 400);
  const second = await request('/api/login', { method: 'POST', body: { username: 'rotated_user', password: premium.secret } });
  assert.equal(second.status, 200);
  const newSecret = `test-only-${crypto.randomUUID()}`;
  const rotated = await request(path, { method: 'POST', token: admin.json.token, body: { password: newSecret } });
  assert.equal(rotated.status, 200);
  assert.equal(rotated.json.user.revoked, false);
  assertNoSecrets(rotated.json);
  for (const token of [premium.json.token, second.json.token])
    assert.equal((await request('/api/session', { token })).status, 401);
  assert.equal(env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?').get(premium.id).n, 0);
  assert.equal((await request('/api/login', { method: 'POST', body: { username: 'rotated_user', password: premium.secret } })).status, 401);
  assert.equal((await request('/api/login', { method: 'POST', body: { username: 'rotated_user', password: newSecret } })).status, 200);
  assert.equal((await request('/api/admin/users/owner/password', { method: 'POST', token: admin.json.token, body: { password: newSecret } })).status, 404);
  // Also protect an administrator whose ID happens to use the normal hex format.
  const otherAdmin = 'c'.repeat(32);
  env.DB.sqlite.prepare(`INSERT INTO users
    (id, username, role, password_salt, password_hash, password_iterations, plan, expires_at, revoked, created_at, updated_at)
    SELECT ?, 'other_admin', role, password_salt, password_hash, password_iterations, plan, expires_at, revoked, created_at, updated_at
    FROM users WHERE id = 'owner'`).run(otherAdmin);
  assert.equal((await request(`/api/admin/users/${otherAdmin}/password`, { method: 'POST', token: admin.json.token, body: { password: newSecret } })).status, 404);
  assert.equal((await request(`/api/admin/users/${otherAdmin}/revocation`, { method: 'POST', token: admin.json.token, body: { revoked: true } })).status, 404);
  assert.equal((await request('/api/session', { token: admin.json.token })).status, 200);
});

test('login credential check cannot race account revocation or password rotation', async () => {
  for (const action of ['revocation', 'password']) {
    const { env, request, owner, member } = fixture();
    const admin = await owner();
    const premium = await member(admin.json.token, 'racing_user');
    const newSecret = `test-only-${crypto.randomUUID()}`;
    const prepare = env.DB.prepare.bind(env.DB);
    let armed = true;
    env.DB.prepare = sql => {
      const statement = prepare(sql);
      if (sql.startsWith('INSERT INTO sessions')) {
        const bind = statement.bind.bind(statement);
        statement.bind = (...values) => {
          const bound = bind(...values), run = bound.run.bind(bound);
          bound.run = async () => {
            if (armed) {
              armed = false;
              const update = await request(`/api/admin/users/${premium.id}`, { method: 'PATCH', token: admin.json.token,
                body: action === 'revocation' ? { revoked: true } : { password: newSecret } });
              assert.equal(update.status, 200);
            }
            return run();
          };
          return bound;
        };
      }
      return statement;
    };
    const oldLogin = await request('/api/login', { method: 'POST', body: { username: 'racing_user', password: premium.secret } });
    assert.equal(oldLogin.status, 401);
    assert.equal(oldLogin.json.error, 'INVALID_LOGIN');
    assert.equal(env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?').get(premium.id).n, 0);
    assert.equal((await request('/api/session', { token: premium.json.token })).status, 401);
    if (action === 'password')
      assert.equal((await request('/api/login', { method: 'POST', body: { username: 'racing_user', password: newSecret } })).status, 200);
  }
});

test('concurrent stale account update cannot restore revoked access', async () => {
  const { env, request, owner, member } = fixture();
  const admin = await owner();
  const premium = await member(admin.json.token);
  const batch = env.DB.batch.bind(env.DB);
  let armed = true;
  env.DB.batch = async statements => {
    if (armed && statements[0].sql.startsWith('UPDATE users SET password_salt')) {
      armed = false;
      const invalidated = await request(`/api/admin/users/${premium.id}/revocation`, {
        method: 'POST', token: admin.json.token, body: { revoked: true } });
      assert.equal(invalidated.status, 200);
    }
    return batch(statements);
  };
  const stale = await request(`/api/admin/users/${premium.id}`, { method: 'PATCH', token: admin.json.token, body: { password: `test-only-${crypto.randomUUID()}` } });
  assert.equal(stale.status, 409);
  assert.equal(stale.json.error, 'USER_CONFLICT');
  assert.equal(env.DB.sqlite.prepare('SELECT revoked FROM users WHERE id = ?').get(premium.id).revoked, 1);
  assert.equal((await request('/api/session', { token: premium.json.token })).status, 401);
});

test('failed invalidation audit rolls back the account update and all session removal', async () => {
  const { env, request, owner, member } = fixture();
  const admin = await owner();
  const premium = await member(admin.json.token);
  env.DB.sqlite.exec("CREATE TRIGGER reject_account_audit BEFORE INSERT ON audit_events BEGIN SELECT RAISE(ABORT, 'test-only internal failure'); END;");
  const response = await request(`/api/admin/users/${premium.id}/revocation`, {
    method: 'POST', token: admin.json.token, body: { revoked: true } });
  assert.equal(response.status, 500);
  assert.equal(env.DB.sqlite.prepare('SELECT revoked FROM users WHERE id = ?').get(premium.id).revoked, 0);
  assert.equal((await request('/api/session', { token: premium.json.token })).status, 200);
  assert.equal((await request('/api/catalog', { token: premium.json.token })).status, 200);
  assert.equal(JSON.stringify(response.json).includes('test-only internal failure'), false);
});

test('catalog publishing persists versions, kinds, exact sizes, provenance and optimistic concurrency', async () => {
  const { env, request, owner, member } = fixture();
  const admin = await owner();
  const premium = await member(admin.json.token);
  let result = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
    body: { entries: [entry()], expected_version: 0 } });
  assert.equal(result.status, 200);
  assert.equal(result.json.version, 1);
  const downloadCatalog = await request('/api/catalog', { token: premium.json.token });
  assert.equal(downloadCatalog.json.entries[0].source_kind, 'official');
  assert.equal(downloadCatalog.json.entries[0].size, 4096);
  assert.deepEqual(downloadCatalog.json.entries[0].language, ['pt-BR']);
  result = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
    body: { entries: [entry({ id: 'test-update', kind: 'update', content_type: 0x1e, filename: 'update.pkg', url: 'https://dlpsgame.com/update.pkg', source_kind: 'official' }),
      entry({ id: 'test-dlc', kind: 'dlc', content_type: 0x1b, filename: 'dlc.pkg' })], expected_version: 1 } });
  assert.equal(result.status, 200);
  assert.equal(result.json.count, 3);
  const current = await request('/api/catalog', { token: admin.json.token });
  assert.deepEqual(current.json.entries.map(item => item.kind), ['base', 'update', 'dlc']);
  assert.equal(current.json.entries[1].source_kind, 'community');
  assert.equal(env.DB.sqlite.prepare("SELECT COUNT(*) AS n FROM audit_events WHERE action = 'catalog.publish'").get().n, 2);
  result = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
    body: { entries: [], expected_version: 1, replace: true } });
  assert.equal(result.status, 409);
  assert.equal((await request('/api/catalog', { token: admin.json.token })).json.entries.length, 3);
  // Fresh worker invocation uses the same D1 data, not in-memory auth/catalog state.
  const direct = await worker.fetch(new Request(`${endpoint}/api/catalog`, { headers: { Authorization: `Bearer ${premium.json.token}` } }), env);
  assert.equal((await direct.json()).version, 2);
});

test('only reviewed public MediaFire and PKG-Zone download formats are accepted; no remote fetch occurs', async () => {
  const { request, owner } = fixture();
  const admin = await owner();
  const fetch = globalThis.fetch;
  let fetches = 0;
  globalThis.fetch = async () => { fetches++; throw Error('Unexpected remote fetch'); };
  try {
    const accepted = [
      'https://www.mediafire.com/file/ABC123/sample.pkg/file',
      'https://mediafire.com/file/abc123/Game%20%5BPS4%5D%20%281.00%29.PKG/file',
      'https://www.mediafire.com/file/A1/Game[PS4]~release-1.pkg/file',
      'https://pkg-zone.com/download/ps4/CUSA01116/latest',
      'https://pkg-zone.com/download/ps4/CUSA00127/latest',
      'https://pkg-zone.com/download/ps4/CUSA02644/latest',
      'https://example.com/sample.pkg',
    ];
    for (let index = 0; index < accepted.length; index++) {
      const result = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
        body: { entries: [entry({ id: `accepted-${index}`, filename: `accepted-${index}.pkg`, url: accepted[index] })], replace: true } });
      assert.equal(result.status, 200, accepted[index]);
      const current = await request('/api/catalog', { token: admin.json.token });
      assert.equal(current.json.entries[0].url, accepted[index]);
      if (index < 6) assert.equal(current.json.entries[0].source_kind, 'community');
    }
    const rejected = [
      'https://pkg-zone.com/download/ps5/CUSA00127/latest',
      'https://pkg-zone.com/download/ps4/CUSA00127/1.53',
      'https://pkg-zone.com/download/ps4/CUSA00127/latest?x=1',
      'https://pkg-zone.com/download/ps4/CUSA00127/latest?',
      'https://pkg-zone.com/download/ps4/cusa00127/latest',
      'https://pkg-zone.com/download/ps4/CUSA00127/latest.pkg',
      'https://pkg-zone.com.evil.example/download/ps4/CUSA00127/latest',
      'https://www.pkg-zone.com/download/ps4/CUSA00127/latest',
      'https://www.mediafire.com.evil.example/file/ABC123/sample.pkg/file',
      'https://files.mediafire.com/file/ABC123/sample.pkg/file',
      'https://example.com/file/ABC123/sample.pkg/file',
      'https://www.mediafire.com/download/ABC123/sample.pkg/file',
      'https://www.mediafire.com/download/ABC123/sample.pkg',
      'https://www.mediafire.com/file/ABC123/sample.pkg',
      'https://www.mediafire.com/file/ABC-123/sample.pkg/file',
      'https://www.mediafire.com/file//sample.pkg/file',
      'https://www.mediafire.com/file/ABC123/sample.rar/file',
      'https://www.mediafire.com/file/ABC123/sample.pkg.html/file',
      'https://www.mediafire.com/file/ABC123/.pkg/file',
      'https://www.mediafire.com/file/ABC123/sample.pkg/FILE',
      'https://www.mediafire.com/file/ABC123/sample.pkg/file/extra',
      'https://www.mediafire.com/file/ABC123/sample.pkg/file?key=abc',
      'https://www.mediafire.com/file/ABC123/sample.pkg/file?',
      'https://www.mediafire.com/file/ABC123/dir%2Fsample.pkg/file',
      'https://www.mediafire.com/file/ABC123/dir%5Csample.pkg/file',
      'https://www.mediafire.com/file/ABC123/sample%00.pkg/file',
      'https://www.mediafire.com/file/ABC123/sample%7F.pkg/file',
      'https://www.mediafire.com/file/ABC123/sample%C3%A1.pkg/file',
      'https://www.mediafire.com/file/ABC123/sample%zz.pkg/file',
      'https://dlpsgame.com/test-game-download.html',
      'https://www.superpsx.com/test-game/',
    ];
    for (const url of rejected) {
      const result = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
        body: { entries: [entry({ url })] } });
      assert.equal(result.status, 400, url);
    }
    assert.equal(fetches, 0);
  } finally { globalThis.fetch = fetch; }
});

test('reviewed author defaults supplement rather than replace the deployment allowlist', async () => {
  const { request, owner } = fixture();
  const admin = await owner();
  const repositories = [
    ['MDashK/Relic-Hunters-Zero-PS4', 'official'],
    ['F1R3xS1NN3R/sound-of-nature', 'official'],
    ['iHaiDeeZ/shattered-pixel-dungeon-ps4', 'official'],
    ['Xyhlo/SSPI', 'official'],
    ['thcolin/gamepad-media-center-aggregator', 'official'],
    ['ScratchEverywhere/ScratchEverywhere', 'official'],
    ['xfangfang/wiliwili', 'official'],
    ['test-author/test-game', 'official'],
    ['unreviewed/example', 'community'],
  ];
  const result = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
    body: { entries: repositories.map(([repository], index) => entry({ id: `author-${index}`, filename: `author-${index}.pkg`,
      url: `https://github.com/${repository}/releases/download/v1/author.pkg` })), replace: true } });
  assert.equal(result.status, 200);
  const current = await request('/api/catalog', { token: admin.json.token });
  assert.deepEqual(current.json.entries.map(item => item.source_kind), repositories.map(([, kind]) => kind));
});

test('untrusted catalog input is rejected before storage; server never fetches remote URLs', async () => {
  const { request, owner } = fixture();
  const admin = await owner();
  const invalid = [
    { size: 1079 }, { size: 256 * 1024 ** 3 + 1 }, { size: 1.5 },
    { url: 'http://example.com/a.pkg' }, { url: 'https://127.0.0.1/a.pkg' },
    { url: 'https://2130706433/a.pkg' }, { url: 'https://[::1]/a.pkg' },
    { url: 'https://foo.local/a.pkg' }, { url: 'https://example.com:8443/a.pkg' },
    { url: 'https://u:p@example.com/a.pkg' }, { url: 'https://example.com/a.pkg#x' },
    { url: 'https://example.com/redirect.html' }, { url: 'https://example.com/%zz.pkg' },
    { url: 'https://example.com\\x/a.pkg' }, { filename: '../x.pkg' },
    { filename: '.hidden.pkg' }, { filename: 'A.PKG' }, { filename: 'a'.repeat(92) + '.pkg' },
    { kind: 'ps5' }, { platform: 'ps3' }, { content_id: 'invalid' },
    { content_id: undefined }, { content_type: undefined }, { content_flags: -1 },
    { content_type: 0x1a, content_flags: 0 }, { kind: 'update' },
    { content_id: 'IV0000-PPSA00001_00-GAME204800000000' },
    { content_type: 0x1b, content_flags: 0x61300000 }, { content_type: 0x1b, kind: 'dlc', is_theme: true },
    { content_type: 0x1c, kind: 'theme', iro_tag: 1, is_theme: true },
    { content_type: 0x1c, kind: 'theme', iro_tag: 2, is_theme: true },
    { version: 'v'.repeat(41) },
    { content_id: 'IV0000-BREW00001_00-GAME204800000000' }, { sha256: 'incorrect' },
    { name: '🎮'.repeat(70) }, { language: ['Português Brasileiro'] },
    { name: 'x\u007f' }, { name: 'x\u0080' }, { version: '\ud800' },
    { requires_data: '\u202e' }, { description: '\u2066' }, { name: '\udc00' },
  ];
  const originalFetch = globalThis.fetch;
  let remoteFetches = 0;
  globalThis.fetch = async () => { remoteFetches++; throw new Error('No remote fetch should occur'); };
  try {
    for (const overrides of invalid) {
      const result = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token, body: { entries: [entry(overrides)] } });
      assert.equal(result.status, 400, JSON.stringify(overrides));
    }
    const valid = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
      body: { entries: [entry({ url: 'https://github.com/test-author/test-game/releases/download/v1/test.PKG' })] } });
    assert.equal(valid.status, 200);
    assert.equal(remoteFetches, 0);
  } finally { globalThis.fetch = originalFetch; }
  const duplicate = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token, body: { entries: [entry(), entry()] } });
  assert.equal(duplicate.status, 400);
  const localCollision = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token, body: { entries: [entry({ id: 'another-id' })] } });
  assert.equal(localCollision.status, 400);
});

test('base/update/DLC/theme derived header policy is preserved exactly', async () => {
  const { request, owner } = fixture();
  const admin = await owner();
  const valid = [
    entry({ id: 'base-homebrew', kind: 'homebrew', filename: 'homebrew.pkg' }),
    entry({ id: 'base-media', kind: 'media', filename: 'media.pkg' }),
    entry({ id: 'patch', kind: 'update', content_type: 0x1a, content_flags: 0x6a700000, filename: 'patch.pkg' }),
    entry({ id: 'dlc', kind: 'dlc', content_type: 0x1c, filename: 'dlc.pkg' }),
    entry({ id: 'theme', kind: 'theme', content_type: 0x1b, iro_tag: 1, is_theme: true, filename: 'theme.pkg', adult: true }),
    entry({ id: 'al-iro-one', kind: 'dlc', content_type: 0x1c, iro_tag: 1, is_theme: false, filename: 'al-one.pkg' }),
    entry({ id: 'al-iro-two', kind: 'dlc', content_type: 0x1c, iro_tag: 2, is_theme: false, filename: 'al-two.pkg' }),
    entry({ id: 'system-theme', kind: 'theme', content_type: 0x1b, iro_tag: 2, is_theme: true, filename: 'system-theme.pkg' }),
    entry({ id: 'sharefactory-custom-note', kind: 'theme', content_type: 0x1b, iro_tag: 1, is_theme: true,
      filename: 'sharefactory-note.pkg', requires_data: 'Requisitos documentados pelo autor.' }),
  ];
  const saved = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token, body: { entries: valid } });
  assert.equal(saved.status, 200);
  const listed = (await request('/api/catalog', { token: admin.json.token })).json.entries;
  assert.equal(listed[2].content_flags, 0x6a700000);
  assert.equal(listed[4].is_theme, true);
  assert.equal(listed[4].iro_tag, 1);
  assert.equal(listed[4].adult, true);
  assert.equal(listed[4].requires_data, 'Tema do SHAREfactory: exige o SHAREfactory instalado para usar o conteúdo.');
  assert.equal(listed[5].is_theme, false);
  assert.equal(listed[5].kind, 'dlc');
  assert.equal(listed[6].is_theme, false);
  assert.equal(listed[6].kind, 'dlc');
  assert.equal(listed[7].is_theme, true);
  assert.equal(listed[7].requires_data, '');
  assert.equal(listed[8].requires_data, 'Requisitos documentados pelo autor.');
});

test('schema-only deployment initializes singleton; concurrent publications keep one winner', async () => {
  const { env, request, owner } = fixture();
  env.DB.sqlite.prepare('DELETE FROM catalog_state').run();
  const admin = await owner();
  const empty = await request('/api/catalog', { token: admin.json.token });
  assert.deepEqual(empty.json, { version: 0, updated_at: 0, entries: [] });
  const publish = entries => request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
    body: { expected_version: 0, entries } });
  const responses = await Promise.all([
    publish([entry({ id: 'first', filename: 'first.pkg' })]),
    publish([entry({ id: 'second', filename: 'second.pkg' })]),
  ]);
  assert.deepEqual(responses.map(response => response.status).sort(), [200, 409]);
  const stored = await request('/api/catalog', { token: admin.json.token });
  assert.equal(stored.json.version, 1);
  assert.equal(stored.json.entries.length, 1);
  assert.equal(env.DB.sqlite.prepare("SELECT COUNT(*) AS n FROM audit_events WHERE action = 'catalog.publish'").get().n, 1);
});

test('actual initial premium seed publishes with release labels and canonical native metadata', async () => {
  const seed = JSON.parse(readFileSync(new URL('../seed-catalog.json', import.meta.url), 'utf8'));
  assert.ok(Array.isArray(seed) && seed.length > 0);
  const { env, request, owner } = fixture();
  env.OFFICIAL_REPOSITORIES = [
    'EmiiBytee/Touhou-PS4', 'MDashK/Sonic-2-SMS-Remake-PS4', 'MDashK/Sonic-Time-Twisted-PS4',
    'MDashK/sonic-1-sms-remake-ps4', 'alechurri/2ship2harkinian-ps4',
    'alechurri/shipofharkinian-ps4', 'iHaiDeeZ/mari0-ps4', 'jaca772/fallout2-ce-ps4',
    'lorsanta/SDLPoP-PS4', 'skidgfx/PS4-2048',
  ].join(',');
  const admin = await owner();
  const result = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token,
    body: { entries: seed, expected_version: 0, replace: true } });
  assert.equal(result.status, 200, JSON.stringify(result.json));
  assert.equal(result.json.count, seed.length);
  const published = (await request('/api/catalog', { token: admin.json.token })).json.entries;
  assert.equal(published.length, seed.length);
  for (let index = 0; index < seed.length; index++) {
    assert.equal(published[index].version, seed[index].version);
    assert.equal(published[index].content_id, seed[index].content_id);
    assert.equal(published[index].content_type, seed[index].content_type);
    assert.equal(published[index].content_flags, seed[index].content_flags);
    assert.equal(published[index].source_kind, seed[index].source_kind);
  }
});

test('account passwords reject unrepresentable C-string/Unicode data without creating users', async () => {
  const { env, request, owner } = fixture();
  const admin = await owner();
  for (const secret of ['password\u0000tail', 'password\ud800', 'password\udc00', 'a'.repeat(129)]) {
    const result = await request('/api/admin/users', { method: 'POST', token: admin.json.token,
      body: { username: 'invalid_credential', password: secret, plan: '15d' } });
    assert.equal(result.status, 400);
  }
  assert.equal(env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM users').get().n, 1);
});

test('failed audit rolls back account/catalog/release mutations and exposes no SQL error', async () => {
  const { env, request, owner } = fixture();
  const admin = await owner();
  env.DB.sqlite.exec("CREATE TRIGGER reject_audit BEFORE INSERT ON audit_events BEGIN SELECT RAISE(ABORT, 'test-only internal failure'); END;");
  const create = await request('/api/admin/users', { method: 'POST', token: admin.json.token,
    body: { username: 'blocked_user', password: `test-only-${crypto.randomUUID()}`, plan: '15d' } });
  assert.equal(create.status, 500);
  assert.equal(env.DB.sqlite.prepare("SELECT COUNT(*) AS n FROM users WHERE username = 'blocked_user'").get().n, 0);
  const publish = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token, body: { entries: [entry()] } });
  assert.equal(publish.status, 500);
  assert.equal((await request('/api/catalog', { token: admin.json.token })).json.version, 0);
  const release = await request('/api/admin/releases', { method: 'POST', token: admin.json.token,
    body: { channel: 'normal', version: 'next', size: 4096, url: 'https://example.com/store.pkg', sha256: 'a'.repeat(64) } });
  assert.equal(release.status, 500);
  assert.deepEqual((await request('/api/releases')).json, { releases: [] });
  assert.equal(JSON.stringify([create.json, publish.json, release.json]).includes('test-only internal failure'), false);
});

test('uniform bad-login errors, persisted username/IP limits, invalid tokens and logout', async () => {
  const { env, request, owner, member } = fixture();
  const admin = await owner();
  const premium = await member(admin.json.token);
  const wrong = await request('/api/login', { method: 'POST', body: { username: 'test_member', password: `wrong-${crypto.randomUUID()}` } });
  const missing = await request('/api/login', { method: 'POST', body: { username: 'missing_user', password: `wrong-${crypto.randomUUID()}` } });
  assert.equal(wrong.status, 401);
  assert.deepEqual(wrong.json, missing.json);
  for (let i = 0; i < 10; i++) {
    const result = await request('/api/login', { method: 'POST', body: { username: 'rate_limited', password: `wrong-${crypto.randomUUID()}` } });
    assert.equal(result.status, 401);
  }
  const limited = await request('/api/login', { method: 'POST', body: { username: 'rate_limited', password: `wrong-${crypto.randomUUID()}` } });
  assert.equal(limited.status, 429);
  assert.equal(limited.response.headers.get('Retry-After'), '600');
  assert.ok(env.DB.sqlite.prepare('SELECT COUNT(*) AS n FROM login_limits').get().n > 0);
  assert.equal((await request('/api/session', { token: '0'.repeat(64) })).status, 401);
  assert.equal((await request(`/api/session?token=${premium.json.token}`)).status, 401);
  assert.equal((await request('/api/logout', { method: 'POST', token: premium.json.token })).status, 200);
  assert.equal((await request('/api/session', { token: premium.json.token })).status, 401);
  env.DB.sqlite.prepare('UPDATE sessions SET expires_at = ?').run(clock);
  assert.equal((await request('/api/session', { token: admin.json.token })).status, 401);
});

test('JSON/stream body limits, CORS allowlist and cookie mutation origin checks', async () => {
  const { env, request, owner } = fixture();
  const admin = await owner();
  const cookie = admin.response.headers.get('Set-Cookie').split(';')[0];
  assert.equal((await request('/api/logout', { method: 'POST', headers: { Cookie: cookie } })).status, 403);
  assert.equal((await request('/api/logout', { method: 'POST', headers: { Cookie: cookie, Origin: 'https://evil.example' } })).status, 403);
  const preflight = await request('/api/login', { method: 'OPTIONS', headers: { Origin: 'https://peppy.example' } });
  assert.equal(preflight.status, 204);
  assert.equal(preflight.response.headers.get('Access-Control-Allow-Origin'), 'https://peppy.example');
  const wrongType = await worker.fetch(new Request(`${endpoint}/api/login`, { method: 'POST', body: '{}' }), env);
  assert.equal(wrongType.status, 415);
  const malformed = await worker.fetch(new Request(`${endpoint}/api/login`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{' }), env);
  assert.equal(malformed.status, 400);
  const large = await worker.fetch(new Request(`${endpoint}/api/login`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: ' '.repeat(17000) }), env);
  assert.equal(large.status, 413);
  const limit = await request('/api/admin/catalog', { method: 'POST', token: admin.json.token, body: { entries: Array.from({ length: 2001 }, (_, i) => entry({ id: `item-${i}` })) } });
  assert.equal(limit.status, 400);
  const validLogout = await request('/api/logout', { method: 'POST', headers: { Cookie: cookie, Origin: endpoint } });
  assert.equal(validLogout.status, 200);
});

test('public releases include store PKGs only; publication requires administrator and checksum', async () => {
  const { request, owner, member } = fixture();
  assert.deepEqual((await request('/api/releases')).json, { releases: [] });
  const admin = await owner();
  const premium = await member(admin.json.token);
  const release = { channel: 'normal', version: 'next', url: 'https://github.com/test-author/store/releases/download/next/store.pkg', size: 8192, sha256: 'a'.repeat(64) };
  assert.equal((await request('/api/admin/releases', { method: 'POST', token: premium.json.token, body: release })).status, 403);
  assert.equal((await request('/api/admin/releases', { method: 'POST', token: admin.json.token, body: { ...release, sha256: undefined } })).status, 400);
  assert.equal((await request('/api/admin/releases', { method: 'POST', token: admin.json.token, body: release })).status, 200);
  const available = await request('/api/releases');
  assert.equal(available.status, 200);
  assert.equal(available.json.releases[0].url, release.url);
  assert.equal('entries' in available.json, false);
  assertNoSecrets(available.json);
});
