-- Auth Service schema (E2E auto-initialization)
-- Based on: apex_services/auth-svc/migrations/

CREATE SCHEMA IF NOT EXISTS auth_svc;

-- Users table
CREATE TABLE IF NOT EXISTS auth_svc.users (
    id              BIGSERIAL       PRIMARY KEY,
    email           VARCHAR(255)    NOT NULL UNIQUE,
    password_hash   VARCHAR(72)     NOT NULL,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    locked_until    TIMESTAMPTZ,
    failed_attempts SMALLINT        NOT NULL DEFAULT 0,

    CONSTRAINT users_email_check CHECK (
        email ~* '^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$'
    )
);

-- Auto-update trigger
CREATE OR REPLACE FUNCTION auth_svc.update_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_users_updated_at
    BEFORE UPDATE ON auth_svc.users
    FOR EACH ROW
    EXECUTE FUNCTION auth_svc.update_updated_at();

-- Refresh tokens table
CREATE TABLE IF NOT EXISTS auth_svc.refresh_tokens (
    id              BIGSERIAL       PRIMARY KEY,
    token_hash      VARCHAR(64)     NOT NULL UNIQUE,
    user_id         BIGINT          NOT NULL REFERENCES auth_svc.users(id) ON DELETE CASCADE,
    expires_at      TIMESTAMPTZ     NOT NULL,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    revoked_at      TIMESTAMPTZ,
    replaced_by     BIGINT          REFERENCES auth_svc.refresh_tokens(id),
    token_family    VARCHAR(64)     NOT NULL DEFAULT gen_random_uuid()::text,

    CONSTRAINT refresh_tokens_expiry_check CHECK (expires_at > created_at)
);

CREATE INDEX IF NOT EXISTS idx_refresh_tokens_user_id
    ON auth_svc.refresh_tokens(user_id) WHERE revoked_at IS NULL;

CREATE INDEX IF NOT EXISTS idx_refresh_tokens_expires_at
    ON auth_svc.refresh_tokens(expires_at) WHERE revoked_at IS NULL;

-- Test seed data: users only (password_hash set by Auth Service on first login attempt)
-- Auth Service's bundled bcrypt generates hashes at startup via seed_test_users().
INSERT INTO auth_svc.users (email, password_hash)
VALUES
    ('alice@apex.dev', 'PENDING'),
    ('bob@apex.dev', 'PENDING'),
    ('charlie@apex.dev', 'PENDING')
ON CONFLICT (email) DO NOTHING;
