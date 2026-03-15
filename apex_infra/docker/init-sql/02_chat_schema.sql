-- Chat Service schema (E2E auto-initialization)
-- Based on: apex_services/chat-svc/sql/

CREATE SCHEMA IF NOT EXISTS chat_svc;

-- Chat rooms table
CREATE TABLE IF NOT EXISTS chat_svc.chat_rooms (
    room_id       BIGSERIAL     PRIMARY KEY,
    room_name     VARCHAR(100)  NOT NULL,
    owner_id      BIGINT        NOT NULL,
    max_members   INTEGER       NOT NULL DEFAULT 100,
    is_active     BOOLEAN       NOT NULL DEFAULT TRUE,
    created_at    TIMESTAMPTZ   NOT NULL DEFAULT NOW(),
    updated_at    TIMESTAMPTZ   NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_chat_rooms_active
    ON chat_svc.chat_rooms (is_active)
    WHERE is_active = TRUE;

-- Chat messages table (history)
CREATE TABLE IF NOT EXISTS chat_svc.chat_messages (
    message_id    BIGSERIAL     PRIMARY KEY,
    room_id       BIGINT        NOT NULL REFERENCES chat_svc.chat_rooms(room_id),
    sender_id     BIGINT        NOT NULL,
    sender_name   VARCHAR(50)   NOT NULL,
    content       TEXT          NOT NULL,
    message_type  SMALLINT      NOT NULL DEFAULT 0,
    created_at    TIMESTAMPTZ   NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_chat_messages_room_time
    ON chat_svc.chat_messages (room_id, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_chat_messages_sender
    ON chat_svc.chat_messages (sender_id, created_at DESC);
