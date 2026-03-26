// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
// context, dockerfile 모두 CWD 기준 상대 경로.
// CI에서는 repo root에서 실행: docker buildx bake -f apex_infra/docker/docker-bake.hcl

variable "CI_IMAGE_TAG" {
  default = "latest"
}

variable "REGISTRY" {
  default = "ghcr.io/gazuua"
}

variable "SHA_TAG" {
  default = "latest"
}

variable "IS_MAIN" {
  default = "false"
}

variable "CMAKE_PRESET" {
  default = "debug"
}

// ── Service group ─────────────────────────────────
group "services" {
  targets = ["gateway", "auth-svc", "chat-svc"]
}

// ── Base target (shared config) ───────────────────
target "service-base" {
  dockerfile = "apex_infra/docker/service.Dockerfile"
  context    = "."
  args = {
    CMAKE_PRESET = CMAKE_PRESET
    CI_IMAGE_TAG = CI_IMAGE_TAG
  }
  cache-from = ["type=registry,ref=${REGISTRY}/apex-pipeline-cache:buildcache"]
  cache-to   = ["type=registry,ref=${REGISTRY}/apex-pipeline-cache:buildcache,mode=max"]
}

// ── Gateway ───────────────────────────────────────
target "gateway" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "apex_gateway"
    SERVICE_DIR  = "gateway"
    CONFIG_FILE  = "gateway_e2e.toml"
  }
  tags = notequal(IS_MAIN, "true") ? [
    "${REGISTRY}/apex-pipeline-gateway:${SHA_TAG}",
  ] : [
    "${REGISTRY}/apex-pipeline-gateway:${SHA_TAG}",
    "${REGISTRY}/apex-pipeline-gateway:latest",
  ]
}

// ── Auth Service ──────────────────────────────────
target "auth-svc" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "auth_svc_main"
    SERVICE_DIR  = "auth-svc"
    CONFIG_FILE  = "auth_svc_e2e.toml"
  }
  tags = notequal(IS_MAIN, "true") ? [
    "${REGISTRY}/apex-pipeline-auth-svc:${SHA_TAG}",
  ] : [
    "${REGISTRY}/apex-pipeline-auth-svc:${SHA_TAG}",
    "${REGISTRY}/apex-pipeline-auth-svc:latest",
  ]
}

// ── Chat Service ──────────────────────────────────
target "chat-svc" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "chat_svc_main"
    SERVICE_DIR  = "chat-svc"
    CONFIG_FILE  = "chat_svc_e2e.toml"
  }
  tags = notequal(IS_MAIN, "true") ? [
    "${REGISTRY}/apex-pipeline-chat-svc:${SHA_TAG}",
  ] : [
    "${REGISTRY}/apex-pipeline-chat-svc:${SHA_TAG}",
    "${REGISTRY}/apex-pipeline-chat-svc:latest",
  ]
}
