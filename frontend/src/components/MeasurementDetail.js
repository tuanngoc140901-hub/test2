import React, { useEffect, useState } from 'react';
import { Button, Card, Descriptions, Input, message, Spin } from 'antd';
import { Radar } from 'react-chartjs-2';
import { Chart as ChartJS, RadialLinearScale, PointElement, LineElement, Filler, Tooltip, Legend } from 'chart.js';
import { fetchMeasurement, updateMeasurementNote } from '../api';

ChartJS.register(RadialLinearScale, PointElement, LineElement, Filler, Tooltip, Legend);

const MeasurementDetail = ({ id, onBack }) => {
  const [measurement, setMeasurement] = useState(null);
  const [loading, setLoading] = useState(true);
  const [note, setNote] = useState('');

  useEffect(() => {
    fetchMeasurement(id)
      .then(res => {
        setMeasurement(res.data);
        setNote(res.data.note || '');
        setLoading(false);
      })
      .catch(() => setLoading(false));
  }, [id]);

  const handleSaveNote = async () => {
    try {
      await updateMeasurementNote(id, note);
      message.success('Đã lưu ghi chú');
    } catch {
      message.error('Lỗi');
    }
  };

  if (loading) return <Spin />;
  if (!measurement) return <p>Không tìm thấy dữ liệu</p>;

  const gasValues = measurement.gas_values || [];
  const radarData = {
    labels: gasValues.map((_, i) => `Sensor ${i+1}`),
    datasets: [{
      label: 'Tín hiệu',
      data: gasValues,
      backgroundColor: 'rgba(255,99,132,0.2)',
      borderColor: 'rgba(255,99,132,1)',
      pointBackgroundColor: 'rgba(255,99,132,1)',
    }]
  };

  return (
    <div>
      <Button onClick={onBack} style={{ marginBottom: 16 }}>Quay lại</Button>
      <Card title={`Chi tiết lần đo #${id}`}>
        <Descriptions bordered column={2}>
          <Descriptions.Item label="Thời gian">{new Date(measurement.ts).toLocaleString()}</Descriptions.Item>
          <Descriptions.Item label="Thiết bị">{measurement.device_id}</Descriptions.Item>
          <Descriptions.Item label="Nhiệt độ">{measurement.temperature?.toFixed(2)} °C</Descriptions.Item>
          <Descriptions.Item label="Độ ẩm">{measurement.humidity?.toFixed(2)} %</Descriptions.Item>
        </Descriptions>
        {gasValues.length > 0 && (
          <div style={{ width: 400, margin: '20px auto' }}>
            <Radar data={radarData} />
          </div>
        )}
        <div style={{ marginTop: 20 }}>
          <Input.TextArea
            rows={3}
            value={note}
            onChange={e => setNote(e.target.value)}
            placeholder="Ghi chú cho lần đo..."
          />
          <Button type="primary" onClick={handleSaveNote} style={{ marginTop: 8 }}>Lưu ghi chú</Button>
        </div>
      </Card>
    </div>
  );
};

export default MeasurementDetail;
