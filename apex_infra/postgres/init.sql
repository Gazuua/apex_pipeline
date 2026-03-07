-- Apex Pipeline: 서비스별 스키마 초기화
-- docker-entrypoint-initdb.d에 마운트되어 최초 기동 시 자동 실행

CREATE SCHEMA IF NOT EXISTS auth_schema;
CREATE SCHEMA IF NOT EXISTS chat_schema;
CREATE SCHEMA IF NOT EXISTS match_schema;

-- 향후 서비스별 DB 유저 분리 시 권한 부여 예시:
-- GRANT USAGE ON SCHEMA auth_schema TO auth_user;
-- GRANT ALL ON ALL TABLES IN SCHEMA auth_schema TO auth_user;
