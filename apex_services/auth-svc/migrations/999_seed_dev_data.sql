-- Development/test seed data -- DO NOT USE IN PRODUCTION
SET search_path TO auth_svc;

-- bcrypt hash of "password123" (work factor 12)
-- Actual hash generated at runtime. This is a valid placeholder hash.
INSERT INTO users (email, password_hash)
VALUES
    ('test@apex.dev', '$2a$12$LJ3m4ys3Lg7EvKmMxOlKCeEJvGMVXyFBQLlKP2cSsYMxR0CKFBkBy'),
    ('admin@apex.dev', '$2a$12$LJ3m4ys3Lg7EvKmMxOlKCeEJvGMVXyFBQLlKP2cSsYMxR0CKFBkBy')
ON CONFLICT (email) DO NOTHING;
