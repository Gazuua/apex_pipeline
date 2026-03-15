-- users table -- user authentication data
SET search_path TO auth_svc;

CREATE TABLE IF NOT EXISTS users (
    id              BIGSERIAL       PRIMARY KEY,
    email           VARCHAR(255)    NOT NULL UNIQUE,
    password_hash   VARCHAR(72)     NOT NULL,  -- bcrypt hash (60 chars) + margin
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    locked_until    TIMESTAMPTZ,               -- NULL = not locked
    failed_attempts SMALLINT        NOT NULL DEFAULT 0,

    -- Email format validation
    CONSTRAINT users_email_check CHECK (email ~* '^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$')
);

-- Auto-update updated_at trigger
CREATE OR REPLACE FUNCTION auth_svc.update_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_users_updated_at
    BEFORE UPDATE ON users
    FOR EACH ROW
    EXECUTE FUNCTION auth_svc.update_updated_at();

COMMENT ON TABLE users IS 'User authentication data. password_hash is bcrypt.';
COMMENT ON COLUMN users.locked_until IS 'NULL = active. When set, account locked until that time.';
COMMENT ON COLUMN users.failed_attempts IS 'Consecutive failure count. Reset to 0 on success.';
