-- Chat Service tables
-- Depends on: 001_create_schema.sql

-- Chat rooms table
CREATE TABLE IF NOT EXISTS chat_svc.chat_rooms (
    room_id       BIGSERIAL     PRIMARY KEY,
    room_name     VARCHAR(100)  NOT NULL,
    owner_id      BIGINT        NOT NULL,       -- users reference (no cross-schema JOIN -- denormalized)
    max_members   INTEGER       NOT NULL DEFAULT 100,
    is_active     BOOLEAN       NOT NULL DEFAULT TRUE,
    created_at    TIMESTAMPTZ   NOT NULL DEFAULT NOW(),
    updated_at    TIMESTAMPTZ   NOT NULL DEFAULT NOW()
);

-- Index: active room listing optimization
CREATE INDEX IF NOT EXISTS idx_chat_rooms_active
    ON chat_svc.chat_rooms (is_active)
    WHERE is_active = TRUE;

-- Chat messages table (history)
CREATE TABLE IF NOT EXISTS chat_svc.chat_messages (
    message_id    BIGSERIAL     PRIMARY KEY,
    room_id       BIGINT        NOT NULL REFERENCES chat_svc.chat_rooms(room_id),
    sender_id     BIGINT        NOT NULL,       -- denormalized
    sender_name   VARCHAR(50)   NOT NULL,       -- denormalized (prevents JOIN)
    content       TEXT          NOT NULL,
    message_type  SMALLINT      NOT NULL DEFAULT 0,  -- 0=normal, 1=system, 2=whisper
    created_at    TIMESTAMPTZ   NOT NULL DEFAULT NOW()
);

-- Index: per-room latest messages (history paging)
CREATE INDEX IF NOT EXISTS idx_chat_messages_room_time
    ON chat_svc.chat_messages (room_id, created_at DESC);

-- Index: per-sender message lookup
CREATE INDEX IF NOT EXISTS idx_chat_messages_sender
    ON chat_svc.chat_messages (sender_id, created_at DESC);
