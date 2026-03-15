#!/bin/bash
# Apex Pipeline -- E2E Kafka topic initialization
# Usage: docker compose exec kafka bash /init-kafka.sh

set -e

KAFKA_BIN="/opt/kafka/bin"
BOOTSTRAP="localhost:9092"

echo "[init-kafka] Waiting for Kafka broker..."
until "$KAFKA_BIN/kafka-broker-api-versions.sh" --bootstrap-server "$BOOTSTRAP" > /dev/null 2>&1; do
    sleep 2
done
echo "[init-kafka] Kafka is ready."

# topic_name:partitions:replication_factor
TOPICS=(
    "auth.requests:4:1"
    "auth.responses:4:1"
    "chat.requests:4:1"
    "chat.responses:4:1"
    "chat.messages.persist:4:1"
    "chat.messages.persist.dlq:1:1"
    "gateway.responses:4:1"
    "gateway.system:1:1"
)

for TOPIC_SPEC in "${TOPICS[@]}"; do
    IFS=':' read -r TOPIC PARTITIONS REPLICATION <<< "$TOPIC_SPEC"
    "$KAFKA_BIN/kafka-topics.sh" --bootstrap-server "$BOOTSTRAP" \
        --create --if-not-exists \
        --topic "$TOPIC" \
        --partitions "$PARTITIONS" \
        --replication-factor "$REPLICATION"
    echo "[init-kafka] Created topic: $TOPIC (partitions=$PARTITIONS)"
done

echo "[init-kafka] All topics created successfully."
