CREATE TABLE devices (
    id SERIAL PRIMARY KEY,
    device_id VARCHAR(50) UNIQUE NOT NULL,
    name VARCHAR(100),
    location VARCHAR(100),
    created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE measurements (
    id BIGSERIAL PRIMARY KEY,
    device_id INTEGER REFERENCES devices(id),
    ts TIMESTAMPTZ NOT NULL,
    temperature REAL,
    humidity REAL,
    gas_values JSONB,
    raw_temperature REAL,
    raw_humidity REAL,
    raw_gas REAL,
    note TEXT,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_measurements_device_ts ON measurements (device_id, ts DESC);
