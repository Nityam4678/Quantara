CREATE EXTENSION IF NOT EXISTS vector;
CREATE TABLE IF NOT EXISTS quantara_schema_version (
  version INTEGER PRIMARY KEY,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
INSERT INTO quantara_schema_version(version) VALUES (1)
ON CONFLICT (version) DO NOTHING;
