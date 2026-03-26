{{/*
Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

Apex Pipeline — 공통 Helm named templates.
모든 서비스 sub-chart가 이 library chart를 dependency로 참조하여 사용.
*/}}

{{/*
Chart 이름. nameOverride가 있으면 사용.
*/}}
{{- define "apex-common.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
릴리스-차트 풀네임. 63자 제한 (K8s label 제약).
*/}}
{{- define "apex-common.fullname" -}}
{{- if .Values.fullnameOverride }}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- $name := default .Chart.Name .Values.nameOverride }}
{{- if contains $name .Release.Name }}
{{- .Release.Name | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" }}
{{- end }}
{{- end }}
{{- end }}

{{/*
차트 라벨 (helm.sh/chart).
*/}}
{{- define "apex-common.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
표준 라벨셋 (app.kubernetes.io/*).
*/}}
{{- define "apex-common.labels" -}}
helm.sh/chart: {{ include "apex-common.chart" . }}
{{ include "apex-common.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Selector 라벨 (matchLabels용).
*/}}
{{- define "apex-common.selectorLabels" -}}
app.kubernetes.io/name: {{ include "apex-common.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
ServiceAccount 이름.
*/}}
{{- define "apex-common.serviceAccountName" -}}
{{- if .Values.serviceAccount.create }}
{{- default (include "apex-common.fullname" .) .Values.serviceAccount.name }}
{{- else }}
{{- default "default" .Values.serviceAccount.name }}
{{- end }}
{{- end }}

{{/* ===================================================================
    ConfigMap — TOML 설정 파일 임베딩
    =================================================================== */}}
{{- define "apex-common.configmap" -}}
apiVersion: v1
kind: ConfigMap
metadata:
  name: {{ include "apex-common.fullname" . }}-config
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
data:
  {{ .Values.config.fileName }}: |
    {{- .Values.config.content | nindent 4 }}
{{- end }}

{{/* ===================================================================
    Service — ClusterIP + 포트 매핑
    =================================================================== */}}
{{- define "apex-common.service" -}}
apiVersion: v1
kind: Service
metadata:
  name: {{ include "apex-common.fullname" . }}
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
spec:
  type: {{ .Values.service.type | default "ClusterIP" }}
  ports:
    {{- range .Values.service.ports }}
    - name: {{ .name }}
      port: {{ .port }}
      targetPort: {{ .targetPort | default .port }}
      protocol: TCP
    {{- end }}
  selector:
    {{- include "apex-common.selectorLabels" . | nindent 4 }}
{{- end }}

{{/* ===================================================================
    Pod template — Deployment/Rollout 공통 Pod spec
    checksum/config annotation 포함.
    =================================================================== */}}
{{- define "apex-common.podTemplate" -}}
metadata:
  annotations:
    checksum/config: {{ include (print $.Template.BasePath "/configmap.yaml") . | sha256sum }}
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
spec:
  {{- if .Values.serviceAccount.create }}
  serviceAccountName: {{ include "apex-common.serviceAccountName" . }}
  {{- end }}
  containers:
    - name: {{ .Chart.Name }}
      image: "{{ .Values.image.repository }}:{{ .Values.image.tag | default .Chart.AppVersion }}"
      imagePullPolicy: {{ .Values.image.pullPolicy | default "IfNotPresent" }}
      securityContext:
      {{- if .Values.securityContext }}
        {{- toYaml .Values.securityContext | nindent 8 }}
      {{- else }}
        runAsNonRoot: true
        runAsUser: 10001
        runAsGroup: 10001
        allowPrivilegeEscalation: false
        readOnlyRootFilesystem: true
      {{- end }}
      ports:
        {{- range .Values.service.ports }}
        - name: {{ .name }}
          containerPort: {{ .targetPort | default .port }}
          protocol: TCP
        {{- end }}
      {{- if or (and .Values.secrets .Values.secrets.existingSecret) (and .Values.secrets .Values.secrets.data) }}
      envFrom:
        {{- if .Values.secrets.existingSecret }}
        - secretRef:
            name: {{ .Values.secrets.existingSecret }}
        {{- else }}
        - secretRef:
            name: {{ include "apex-common.fullname" . }}-secrets
        {{- end }}
      {{- end }}
      {{- if .Values.extraEnv }}
      env:
        {{- toYaml .Values.extraEnv | nindent 8 }}
      {{- end }}
      volumeMounts:
        - name: config
          mountPath: {{ .Values.config.mountPath }}/config.toml
          subPath: {{ .Values.config.fileName }}
          readOnly: true
        - name: tmp
          mountPath: /tmp
        {{- if .Values.extraVolumeMounts }}
        {{- toYaml .Values.extraVolumeMounts | nindent 8 }}
        {{- end }}
      {{- if .Values.probes }}
      {{- if .Values.probes.startup }}
      startupProbe:
        {{- toYaml .Values.probes.startup | nindent 8 }}
      {{- end }}
      {{- if .Values.probes.liveness }}
      livenessProbe:
        {{- toYaml .Values.probes.liveness | nindent 8 }}
      {{- end }}
      {{- if .Values.probes.readiness }}
      readinessProbe:
        {{- toYaml .Values.probes.readiness | nindent 8 }}
      {{- end }}
      {{- end }}
      {{- if .Values.resources }}
      resources:
        {{- toYaml .Values.resources | nindent 8 }}
      {{- end }}
  volumes:
    - name: config
      configMap:
        name: {{ include "apex-common.fullname" . }}-config
    - name: tmp
      emptyDir: {}
    {{- if .Values.extraVolumes }}
    {{- toYaml .Values.extraVolumes | nindent 4 }}
    {{- end }}
{{- end }}

{{/* ===================================================================
    Deployment — podTemplate을 포함하는 Deployment 리소스.
    rollouts.enabled=true 시 서비스 deployment.yaml에서 가드로 비활성화.
    =================================================================== */}}
{{- define "apex-common.deployment" -}}
apiVersion: apps/v1
kind: Deployment
metadata:
  name: {{ include "apex-common.fullname" . }}
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
spec:
  replicas: {{ .Values.replicaCount | default 1 }}
  selector:
    matchLabels:
      {{- include "apex-common.selectorLabels" . | nindent 6 }}
  template:
    {{- include "apex-common.podTemplate" . | nindent 4 }}
{{- end }}

{{/* ===================================================================
    Rollout — Argo Rollouts CRD (조건부).
    rollouts.enabled=true 시 Deployment 대신 Rollout 리소스 생성.
    =================================================================== */}}
{{- define "apex-common.rollout" -}}
{{- if and .Values.rollouts .Values.rollouts.enabled }}
apiVersion: argoproj.io/v1alpha1
kind: Rollout
metadata:
  name: {{ include "apex-common.fullname" . }}
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
spec:
  replicas: {{ .Values.replicaCount | default 1 }}
  selector:
    matchLabels:
      {{- include "apex-common.selectorLabels" . | nindent 6 }}
  template:
    {{- include "apex-common.podTemplate" . | nindent 4 }}
  strategy:
    canary:
      {{- toYaml .Values.rollouts.canary | nindent 6 }}
{{- end }}
{{- end }}

{{/* ===================================================================
    Secret — secrets.data가 있고 existingSecret이 없을 때 자동 생성
    =================================================================== */}}
{{- define "apex-common.secret" -}}
{{- if and .Values.secrets .Values.secrets.data (not .Values.secrets.existingSecret) }}
apiVersion: v1
kind: Secret
metadata:
  name: {{ include "apex-common.fullname" . }}-secrets
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
type: Opaque
stringData:
  {{- range $key, $value := .Values.secrets.data }}
  {{ $key }}: {{ $value | quote }}
  {{- end }}
{{- end }}
{{- end }}

{{/* ===================================================================
    ServiceAccount (조건부)
    =================================================================== */}}
{{- define "apex-common.serviceAccount" -}}
{{- if .Values.serviceAccount.create }}
apiVersion: v1
kind: ServiceAccount
metadata:
  name: {{ include "apex-common.serviceAccountName" . }}
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
  {{- with .Values.serviceAccount.annotations }}
  annotations:
    {{- toYaml . | nindent 4 }}
  {{- end }}
{{- if .Values.imagePullSecrets }}
imagePullSecrets:
  {{- toYaml .Values.imagePullSecrets | nindent 2 }}
{{- end }}
{{- end }}
{{- end }}

{{/* ===================================================================
    ServiceMonitor — Prometheus CRD (조건부)
    =================================================================== */}}
{{- define "apex-common.serviceMonitor" -}}
{{- if and .Values.serviceMonitor .Values.serviceMonitor.enabled }}
apiVersion: monitoring.coreos.com/v1
kind: ServiceMonitor
metadata:
  name: {{ include "apex-common.fullname" . }}
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
    release: prometheus
spec:
  selector:
    matchLabels:
      {{- include "apex-common.selectorLabels" . | nindent 6 }}
  endpoints:
    - port: {{ .Values.serviceMonitor.port | default "metrics" }}
      path: {{ .Values.serviceMonitor.path | default "/metrics" }}
      interval: {{ .Values.serviceMonitor.interval | default "15s" }}
{{- end }}
{{- end }}
