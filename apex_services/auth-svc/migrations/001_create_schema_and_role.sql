-- Auth Service schema + grants
-- Requires: superuser privileges
-- NOTE: auth_role creation is handled by 001_create_schema_and_role.sh

-- Create schema
CREATE SCHEMA IF NOT EXISTS auth_svc;

-- Grant permissions: auth_svc schema only
GRANT USAGE ON SCHEMA auth_svc TO auth_role;
GRANT CREATE ON SCHEMA auth_svc TO auth_role;

-- Default privileges: auto-grant on all tables created in auth_svc
ALTER DEFAULT PRIVILEGES IN SCHEMA auth_svc
    GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO auth_role;

-- Block public schema access
REVOKE ALL ON SCHEMA public FROM auth_role;

COMMENT ON SCHEMA auth_svc IS 'Auth Service schema -- users, refresh_tokens';
