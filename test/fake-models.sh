#!/bin/sh
# fake /models endpoint for smoke tests: openai page 1, pagination to an
# anthropic-shaped page 2, then done
case "$QUERY_STRING" in
  *"after=gpt-5.2"*)
    printf 'HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n'
    printf '{"data":[{"id":"claude-opus-4-5","type":"model","display_name":"Claude Opus 4.5","created_at":"2025-11-24T00:00:00Z"},{"id":"claude-haiku-4-5","type":"model","display_name":"Claude Haiku 4.5","created_at":"2025-10-15T12:34:56.789Z"}],"has_more":false,"first_id":"claude-haiku-4-5","last_id":"claude-opus-4-5"}'
    ;;
  *)
    printf 'HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n'
    printf '{"object":"list","data":[{"id":"gpt-5.2","object":"model","created":1764115200,"owned_by":"system"},{"id":"o4-mini","object":"model","created":1745337600,"owned_by":"organization-owner"}],"has_more":true,"first_id":"o4-mini","last_id":"gpt-5.2"}'
    ;;
esac
