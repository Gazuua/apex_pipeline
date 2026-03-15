-- refresh_tokens table -- Refresh Token management
SET search_path TO auth_svc;

CREATE TABLE IF NOT EXISTS refresh_tokens (
    id              BIGSERIAL       PRIMARY KEY,
    token_hash      VARCHAR(64)     NOT NULL UNIQUE,  -- SHA-256 hash (hex, 64 chars)
    user_id         BIGINT          NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires_at      TIMESTAMPTZ     NOT NULL,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    revoked_at      TIMESTAMPTZ,                      -- NULL = active, set = revoked
    replaced_by     BIGINT          REFERENCES refresh_tokens(id),  -- New token ID on rotation

    CONSTRAINT refresh_tokens_expiry_check CHECK (expires_at > created_at)
);

-- Index: fast lookup by user_id (active tokens)
CREATE INDEX IF NOT EXISTS idx_refresh_tokens_user_id
    ON refresh_tokens(user_id)
    WHERE revoked_at IS NULL;

-- Index: expired token cleanup
CREATE INDEX IF NOT EXISTS idx_refresh_tokens_expires_at
    ON refresh_tokens(expires_at)
    WHERE revoked_at IS NULL;

COMMENT ON TABLE refresh_tokens IS 'Refresh Token management. token_hash is SHA-256(token) hex.';
COMMENT ON COLUMN refresh_tokens.replaced_by IS 'New token ID on rotation. Used for Refresh Token Reuse Detection.';
