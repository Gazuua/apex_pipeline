-- Chat Service schema + role creation
-- Requires: superuser privileges

-- Create schema
CREATE SCHEMA IF NOT EXISTS chat_svc;

-- Create dedicated role (cross-schema access blocked)
DO $$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'chat_role') THEN
        CREATE ROLE chat_role WITH LOGIN PASSWORD 'chat_secret_change_me';
    END IF;
END
$$;

-- Grant permissions: chat_svc schema only
GRANT USAGE ON SCHEMA chat_svc TO chat_role;
GRANT CREATE ON SCHEMA chat_svc TO chat_role;

-- Default privileges: auto-grant on all tables created in chat_svc
ALTER DEFAULT PRIVILEGES IN SCHEMA chat_svc
    GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO chat_role;

-- Block public schema access
REVOKE ALL ON SCHEMA public FROM chat_role;

COMMENT ON SCHEMA chat_svc IS 'Chat Service schema -- rooms, messages';
