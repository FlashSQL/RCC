-- PostgreSQL version of partitioned table for synthetic benchmark
-- 10만 row * 128 파티션 + 1 hotspot용 파티션

-- Create database (run as superuser)
-- CREATE DATABASE synt;
-- \c synt;

-- Helper tables for data generation
CREATE TABLE IF NOT EXISTS seq1000 (i INT PRIMARY KEY);
INSERT INTO seq1000 (i)
SELECT generate_series(0, 999)
ON CONFLICT (i) DO NOTHING;

CREATE TABLE IF NOT EXISTS seq50 (i INT PRIMARY KEY);
INSERT INTO seq50 (i)
SELECT generate_series(1, 50)
ON CONFLICT (i) DO NOTHING;

CREATE TABLE IF NOT EXISTS seq100 (i INT PRIMARY KEY);
INSERT INTO seq100 (i)
SELECT generate_series(1, 100)
ON CONFLICT (i) DO NOTHING;

-- Drop existing table if exists
DROP TABLE IF EXISTS bench_hotread CASCADE;

-- Create main partitioned table
CREATE TABLE bench_hotread (
  p  INT NOT NULL,                         -- partition key (-1: hotspot, 0..127: read)
  id BIGSERIAL NOT NULL,                   -- auto-incrementing ID
  val BIGINT NOT NULL DEFAULT 0,
  payload BYTEA NOT NULL,
  PRIMARY KEY (id, p)
) PARTITION BY LIST (p);

-- Create partitions
CREATE TABLE bench_hotread_hot PARTITION OF bench_hotread FOR VALUES IN (-1);

-- Create 128 partitions for read workload (0..127)
DO $$
BEGIN
  FOR i IN 0..127 LOOP
    EXECUTE format('CREATE TABLE bench_hotread_p%s PARTITION OF bench_hotread FOR VALUES IN (%s)', i, i);
  END LOOP;
END $$;

-- Insert single hotspot row
INSERT INTO bench_hotread (p, id, val, payload)
VALUES (-1, 1, 0, repeat('x', 2048)::bytea);

-- Generate data for partitions 0..127
-- Each partition gets 100,000 rows (1000 * 100)
INSERT INTO bench_hotread (p, val, payload)
SELECT pset.p, 0, repeat('x', 2048)::bytea
FROM (SELECT i AS p FROM generate_series(0, 127) AS i) AS pset
CROSS JOIN seq1000 a
CROSS JOIN seq100 b;

-- Create indexes on each partition for better performance
DO $$
BEGIN
  -- Index for hotspot partition
  EXECUTE 'CREATE INDEX idx_bench_hotread_hot_id ON bench_hotread_hot(id)';

  -- Indexes for read partitions
  FOR i IN 0..127 LOOP
    EXECUTE format('CREATE INDEX idx_bench_hotread_p%s_id ON bench_hotread_p%s(id)', i, i);
  END LOOP;
END $$;

-- Analyze the table for query optimization
ANALYZE bench_hotread;
