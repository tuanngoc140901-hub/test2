mqttClient.on('message', async (topic, msg) => {
  try {
    const data = JSON.parse(msg);
    const deviceId = data.device_id;

    // Tìm hoặc tạo device
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
    const gasValues = JSON.stringify([data.gas]);

    const result = await pool.query(
      `INSERT INTO measurements(device_id, ts, temperature, humidity, gas_values,
        raw_temperature, raw_humidity, raw_gas)
       VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
       RETURNING id, ts, temperature, humidity, gas_values, raw_temperature, raw_humidity, raw_gas`,
      [devDbId, ts, data.temp, data.hum, gasValues, data.raw_temp, data.raw_hum, data.raw_gas]
    );
    io.emit('new_measurement', result.rows[0]);
  } catch (e) {
    console.error('MQTT message error:', e);
  }
});
