PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS users (
  id TEXT PRIMARY KEY,
  username TEXT NOT NULL UNIQUE,
  role TEXT NOT NULL CHECK (role IN ('admin', 'premium')),
  password_salt TEXT NOT NULL,
  password_hash TEXT NOT NULL,
  password_iterations INTEGER NOT NULL CHECK (password_iterations = 100000),
  plan TEXT CHECK (plan IN ('15d', '1m', '2m')),
  expires_at INTEGER,
  revoked INTEGER NOT NULL DEFAULT 0 CHECK (revoked IN (0, 1)),
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  approved_by TEXT,
  approval_method TEXT NOT NULL DEFAULT 'manual_admin',
  CHECK (role = 'admin' OR (plan IS NOT NULL AND expires_at IS NOT NULL))
);

CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY,
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  expires_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);
CREATE INDEX IF NOT EXISTS sessions_expiration ON sessions(expires_at);

CREATE TABLE IF NOT EXISTS login_limits (
  key TEXT PRIMARY KEY,
  window_start INTEGER NOT NULL,
  attempts INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS catalog_state (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  version INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL DEFAULT 0
);
INSERT OR IGNORE INTO catalog_state (id, version, updated_at) VALUES (1, 0, 0);
CREATE TABLE IF NOT EXISTS catalog_revisions (
  version INTEGER PRIMARY KEY,
  payload TEXT NOT NULL,
  updated_at INTEGER NOT NULL,
  actor_id TEXT NOT NULL REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS releases (
  channel TEXT PRIMARY KEY CHECK (channel IN ('normal', 'premium')),
  payload TEXT NOT NULL,
  updated_at INTEGER NOT NULL,
  actor_id TEXT NOT NULL REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS audit_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  actor_id TEXT NOT NULL REFERENCES users(id),
  action TEXT NOT NULL,
  entity_id TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
