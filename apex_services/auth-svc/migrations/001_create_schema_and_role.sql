-- Auth Service schema + role creation
-- Requires: superuser privileges

-- Create schema
CREATE SCHEMA IF NOT EXISTS auth_svc;

-- Create dedicated role (cross-schema access blocked)
DO $$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'auth_role') THEN
        -- WARNING: Default dev password. MUST be changed in production.
        -- Will migrate to Docker/K8s secrets-based injection in v0.6.
        CREATE ROLE auth_role WITH LOGIN PASSWORD 'auth_secret_change_me';
    END IF;
END
$$;

-- Grant permissions: auth_svc schema only
GRANT USAGE ON SCHEMA auth_svc TO auth_role;
GRANT CREATE ON SCHEMA auth_svc TO auth_role;

-- Default privileges: auto-grant on all tables created in auth_svc
ALTER DEFAULT PRIVILEGES IN SCHEMA auth_svc
    GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO auth_role;

-- Block public schema access
REVOKE ALL ON SCHEMA public FROM auth_role;

COMMENT ON SCHEMA auth_svc IS 'Auth Service schema -- users, refresh_tokens';
