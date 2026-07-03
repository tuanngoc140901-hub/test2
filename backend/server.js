require('dotenv').config();
const express = require('express');
const http = require('http');
const socketIo = require('socket.io');
const mqtt = require('mqtt');
const cors = require('cors');
const pool = require('./db');
const fs = require('fs');
const path = require('path');

const app = express();
const server = http.createServer(app);
const io = socketIo(server, { cors: { origin: '*' } });

app.use(cors());
app.use(express.json());

// ================== CẤU HÌNH FILE CSV & JSON ==================
const CSV_FILE = process.env.CSV_FILE || '/app/data/sensor_data.csv';
const JSON_FILE = process.env.JSON_FILE || '/app/data/sensor_data.json';

function initCSV() {
    const dir = path.dirname(CSV_FILE);
    if (!fs.existsSync(dir)) {
        fs.mkdirSync(dir, { recursive: true });
    }
    if (!fs.existsSync(CSV_FILE)) {
        fs.writeFileSync(CSV_FILE, 'timestamp,device_id,raw_temp,raw_hum,temp,hum\n');
    }
}

function appendCSV(ts, deviceId, rawTemp, rawHum, temp, hum) {
    const line = `${ts},${deviceId},${rawTemp},${rawHum},${temp},${hum}\n`;
    fs.appendFileSync(CSV_FILE, line);
}

function updateJSON(newRecord) {
    let data = [];
    if (fs.existsSync(JSON_FILE)) {
        try {
            const raw = fs.readFileSync(JSON_FILE);
            data = JSON.parse(raw);
        } catch (e) { data = []; }
    }
    data.push(newRecord);
    if (data.length > 1000) {
        data = data.slice(-1000);
    }
    fs.writeFileSync(JSON_FILE, JSON.stringify(data, null, 2));
}

initCSV();

// ================== MQTT CLIENT ==================
const mqttClient = mqtt.connect(process.env.MQTT_BROKER || 'mqtt://mosquitto');
mqttClient.on('connect', () => {
    console.log('MQTT connected');
    mqttClient.subscribe('e-nose/+/data');
});

mqttClient.on('message', async (topic, msg) => {
    try {
        const data = JSON.parse(msg);
        const deviceId = data.device_id;

        const devRes = await pool.query(
            'INSERT INTO devices(device_id) VALUES($1) ON CONFLICT DO NOTHING RETURNING id',
            [deviceId]
        );
        let devDbId = devRes.rows[0]?.id;
        if (!devDbId) {
            const getDev = await pool.query('SELECT id FROM devices WHERE device_id=$1', [deviceId]);
            devDbId = getDev.rows[0].id;
        }

        const ts = data.ts ? new Date(data.ts * 1000).toISOString() : new Date().toISOString();

        const result = await pool.query(
            `INSERT INTO measurements(device_id, ts, temperature, humidity, raw_temperature, raw_humidity)
             VALUES ($1, $2, $3, $4, $5, $6) RETURNING id, ts, temperature, humidity, raw_temperature, raw_humidity`,
            [devDbId, ts, data.temp, data.hum, data.raw_temp, data.raw_hum]
        );
        const newRow = result.rows[0];

        appendCSV(ts, deviceId, data.raw_temp, data.raw_hum, data.temp, data.hum);
        updateJSON({
            ts: ts,
            device_id: deviceId,
            raw_temp: data.raw_temp,
            raw_hum: data.raw_hum,
            temp: data.temp,
            hum: data.hum
        });

        io.emit('new_measurement', newRow);
    } catch (e) {
        console.error('MQTT message error:', e);
    }
});

// ================== API ==================
app.get('/api/measurements', async (req, res) => {
    try {
        const { device_id, from, to, page = 1, limit = 50 } = req.query;
        let query = `SELECT m.id, m.ts, m.temperature, m.humidity, m.raw_temperature, m.raw_humidity, d.device_id AS device_name
                     FROM measurements m JOIN devices d ON m.device_id = d.id WHERE 1=1`;
        const params = [];
        let paramIdx = 1;
        if (device_id) {
            query += ` AND d.device_id = $${paramIdx}`;
            params.push(device_id);
            paramIdx++;
        }
        if (from) {
            query += ` AND m.ts >= $${paramIdx}`;
            params.push(from);
            paramIdx++;
        }
        if (to) {
            query += ` AND m.ts <= $${paramIdx}`;
            params.push(to);
            paramIdx++;
        }
        query += ` ORDER BY m.ts DESC LIMIT $${paramIdx} OFFSET $${paramIdx+1}`;
        params.push(limit, (page-1)*limit);
        const result = await pool.query(query, params);
        res.json(result.rows);
    } catch (err) {
        console.error(err);
        res.status(500).json({ error: 'Internal server error' });
    }
});

app.get('/api/measurements/:id', async (req, res) => {
    try {
        const { id } = req.params;
        const result = await pool.query('SELECT * FROM measurements WHERE id=$1', [id]);
        if (result.rows.length === 0) return res.status(404).json({ error: 'Not found' });
        res.json(result.rows[0]);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

app.patch('/api/measurements/:id', async (req, res) => {
    try {
        const { id } = req.params;
        const { note } = req.body;
        await pool.query('UPDATE measurements SET note=$1 WHERE id=$2', [note, id]);
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

app.get('/api/devices', async (req, res) => {
    try {
        const result = await pool.query('SELECT * FROM devices ORDER BY created_at DESC');
        res.json(result.rows);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Xuất CSV
app.get('/api/export/csv', async (req, res) => {
    try {
        const { device_id, from, to } = req.query;
        let query = `SELECT m.ts, d.device_id, m.raw_temperature, m.raw_humidity, m.temperature, m.humidity
                     FROM measurements m
                     JOIN devices d ON m.device_id = d.id
                     WHERE 1=1`;
        const params = [];
        let paramIdx = 1;
        if (device_id) {
            query += ` AND d.device_id = $${paramIdx}`;
            params.push(device_id);
            paramIdx++;
        }
        if (from) {
            query += ` AND m.ts >= $${paramIdx}`;
            params.push(from);
            paramIdx++;
        }
        if (to) {
            query += ` AND m.ts <= $${paramIdx}`;
            params.push(to);
            paramIdx++;
        }
        query += ` ORDER BY m.ts ASC`;
        const result = await pool.query(query, params);
        const rows = result.rows;

        let csv = 'timestamp,device_id,raw_temp,raw_hum,temp,hum\n';
        rows.forEach(row => {
            csv += `${row.ts},${row.device_id},${row.raw_temperature},${row.raw_humidity},${row.temperature},${row.humidity}\n`;
        });

        res.setHeader('Content-Type', 'text/csv');
        res.setHeader('Content-Disposition', `attachment; filename=sensor_data_${Date.now()}.csv`);
        res.send(csv);
    } catch (err) {
        console.error(err);
        res.status(500).json({ error: err.message });
    }
});

// Xuất JSON
app.get('/api/export/json', async (req, res) => {
    try {
        const { device_id, from, to } = req.query;
        let query = `SELECT m.ts, d.device_id, m.raw_temperature, m.raw_humidity, m.temperature, m.humidity
                     FROM measurements m
                     JOIN devices d ON m.device_id = d.id
                     WHERE 1=1`;
        const params = [];
        let paramIdx = 1;
        if (device_id) {
            query += ` AND d.device_id = $${paramIdx}`;
            params.push(device_id);
            paramIdx++;
        }
        if (from) {
            query += ` AND m.ts >= $${paramIdx}`;
            params.push(from);
            paramIdx++;
        }
        if (to) {
            query += ` AND m.ts <= $${paramIdx}`;
            params.push(to);
            paramIdx++;
        }
        query += ` ORDER BY m.ts ASC`;
        const result = await pool.query(query, params);
        const rows = result.rows;

        res.setHeader('Content-Type', 'application/json');
        res.setHeader('Content-Disposition', `attachment; filename=sensor_data_${Date.now()}.json`);
        res.json(rows);
    } catch (err) {
        console.error(err);
        res.status(500).json({ error: err.message });
    }
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
    console.log(`Backend running on port ${PORT}`);
});
