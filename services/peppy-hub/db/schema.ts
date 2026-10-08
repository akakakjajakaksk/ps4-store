/** Optional Sites/Drizzle mapping. Worker runtime itself has no package dependencies. */
import { sql } from 'drizzle-orm';
import { sqliteTable, text, integer, index, check } from 'drizzle-orm/sqlite-core';

export const users = sqliteTable('users', {
  id: text('id').primaryKey(), username: text('username').notNull().unique(),
  role: text('role', { enum: ['admin', 'premium'] }).notNull(),
  passwordSalt: text('password_salt').notNull(), passwordHash: text('password_hash').notNull(),
  passwordIterations: integer('password_iterations').notNull(),
  plan: text('plan', { enum: ['15d', '1m', '2m'] }), expiresAt: integer('expires_at'),
  revoked: integer('revoked').notNull().default(0), createdAt: integer('created_at').notNull(),
  updatedAt: integer('updated_at').notNull(), approvedBy: text('approved_by'),
  approvalMethod: text('approval_method').notNull().default('manual_admin'),
}, table => [
  check('users_role', sql`${table.role} IN ('admin', 'premium')`),
  check('users_plan', sql`${table.plan} IS NULL OR ${table.plan} IN ('15d', '1m', '2m')`),
  check('users_iterations', sql`${table.passwordIterations} = 100000`),
  check('users_revoked', sql`${table.revoked} IN (0, 1)`),
  check('users_premium_expiration', sql`${table.role} = 'admin' OR (${table.plan} IS NOT NULL AND ${table.expiresAt} IS NOT NULL)`),
]);
export const sessions = sqliteTable('sessions', {
  tokenHash: text('token_hash').primaryKey(), userId: text('user_id').notNull().references(() => users.id, { onDelete: 'cascade' }),
  expiresAt: integer('expires_at').notNull(), createdAt: integer('created_at').notNull(),
}, table => [index('sessions_user').on(table.userId), index('sessions_expiration').on(table.expiresAt)]);
export const loginLimits = sqliteTable('login_limits', {
  key: text('key').primaryKey(), windowStart: integer('window_start').notNull(), attempts: integer('attempts').notNull(),
});
export const catalogState = sqliteTable('catalog_state', {
  id: integer('id').primaryKey(), version: integer('version').notNull().default(0), updatedAt: integer('updated_at').notNull().default(0),
}, table => [check('catalog_singleton', sql`${table.id} = 1`)]);
export const catalogRevisions = sqliteTable('catalog_revisions', {
  version: integer('version').primaryKey(), payload: text('payload').notNull(), updatedAt: integer('updated_at').notNull(),
  actorId: text('actor_id').notNull().references(() => users.id),
});
export const releases = sqliteTable('releases', {
  channel: text('channel', { enum: ['normal', 'premium'] }).primaryKey(),
  payload: text('payload').notNull(), updatedAt: integer('updated_at').notNull(), actorId: text('actor_id').notNull().references(() => users.id),
}, table => [check('releases_channel', sql`${table.channel} IN ('normal', 'premium')`)]);
export const auditEvents = sqliteTable('audit_events', {
  id: integer('id').primaryKey({ autoIncrement: true }), actorId: text('actor_id').notNull().references(() => users.id),
  action: text('action').notNull(), entityId: text('entity_id').notNull(), createdAt: integer('created_at').notNull(),
});
