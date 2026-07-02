import React, { useEffect, useState, useCallback } from 'react';
import { Table, Button, DatePicker, Space, Select, notification } from 'antd';
import { fetchMeasurements, fetchDevices } from '../api';
import moment from 'moment';

const { RangePicker } = DatePicker;

const Dashboard = ({ onViewDetail, realTimeData }) => {
  const [data, setData] = useState([]);
  const [loading, setLoading] = useState(false);
  const [devices, setDevices] = useState([]);
  const [selectedDevice, setSelectedDevice] = useState(null);
  const [page, setPage] = useState(1);
  const [dateRange, setDateRange] = useState([]);
  const [pagination, setPagination] = useState({ current: 1, pageSize: 50, total: 0 });

  const loadData = useCallback(async () => {
    setLoading(true);
    try {
      const params = { page, limit: pagination.pageSize };
      if (selectedDevice) params.device_id = selectedDevice;
      if (dateRange && dateRange.length === 2) {
        params.from = dateRange[0].toISOString();
        params.to = dateRange[1].toISOString();
      }
      const res = await fetchMeasurements(params);
      setData(res.data);
      setPagination(prev => ({ ...prev, current: page }));
    } catch (err) {
      notification.error({ message: 'Lỗi tải dữ liệu' });
    } finally {
      setLoading(false);
    }
  }, [page, selectedDevice, dateRange]);

  useEffect(() => {
    loadData();
  }, [loadData]);

  useEffect(() => {
    if (realTimeData) {
      setData(prev => [realTimeData, ...prev.slice(0, 49)]);
    }
  }, [realTimeData]);

  useEffect(() => {
    fetchDevices().then(res => setDevices(res.data)).catch(() => {});
  }, []);

  const columns = [
  {
    title: 'Thời gian', dataIndex: 'ts', key: 'ts',
    render: text => moment(text).format('HH:mm:ss DD/MM/YY'),
    sorter: (a, b) => moment(a.ts).unix() - moment(b.ts).unix(),
    defaultSortOrder: 'descend',
  },
  { title: 'Nhiệt độ', dataIndex: 'temperature', key: 'temp', render: v => v?.toFixed(1) + ' °C' },
  { title: 'Độ ẩm', dataIndex: 'humidity', key: 'hum', render: v => v?.toFixed(1) + ' %' },
  { title: 'Gas (cal)', dataIndex: 'gas_values', key: 'gas_cal', render: arr => arr?.[0]?.toFixed(1) || '-' },
  { title: 'Raw Temp', dataIndex: 'raw_temperature', key: 'raw_temp', render: v => v?.toFixed(1) + ' °C' },
  { title: 'Raw Hum', dataIndex: 'raw_humidity', key: 'raw_hum', render: v => v?.toFixed(1) + ' %' },
  { title: 'Raw Gas', dataIndex: 'raw_gas', key: 'raw_gas', render: v => v?.toFixed(1) || '-' },
  { title: 'Hành động', render: (_, record) => <Button type="link" onClick={() => onViewDetail(record.id)}>Chi tiết</Button> },
];

  return (
    <div>
      <Space style={{ marginBottom: 16 }}>
        <Select
          placeholder="Chọn thiết bị"
          allowClear
          style={{ width: 200 }}
          onChange={val => setSelectedDevice(val)}
          options={devices.map(d => ({ label: d.device_id, value: d.device_id }))}
        />
        <RangePicker
          showTime={{ format: 'HH:mm' }}
          format="YYYY-MM-DD HH:mm"
          onChange={(dates) => {
            if (dates) setDateRange([dates[0].toDate(), dates[1].toDate()]);
            else setDateRange([]);
          }}
        />
      </Space>
      <Table
        rowKey="id"
        columns={columns}
        dataSource={data}
        loading={loading}
        pagination={{
          ...pagination,
          onChange: (p) => setPage(p),
        }}
      />
    </div>
  );
};

export default Dashboard;
