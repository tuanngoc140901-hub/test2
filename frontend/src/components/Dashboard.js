import React, { useEffect, useState, useCallback } from 'react';
import { Table, Button, DatePicker, Space, Select, notification } from 'antd';
import { DownloadOutlined } from '@ant-design/icons';
import { fetchMeasurements, fetchDevices } from '../api';
import moment from 'moment';

const { RangePicker } = DatePicker;

// Hàm chuyển đổi ngày thành đầu/cuối ngày
const formatDateForAPI = (date, isEndOfDay = false) => {
  if (!date) return null;
  const d = new Date(date);
  if (isEndOfDay) {
    d.setHours(23, 59, 59, 999);
  } else {
    d.setHours(0, 0, 0, 0);
  }
  return d.toISOString();
};

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
        params.from = formatDateForAPI(dateRange[0], false);  // đầu ngày
        params.to   = formatDateForAPI(dateRange[1], true);   // cuối ngày
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
    // 1. Kiểm tra bộ lọc Thiết bị (nếu đang chọn)
    // Lưu ý: Backend emit trả về `device_id` dạng mã gốc hoặc ID, cần khớp với cấu trúc bạn lưu ở `selectedDevice`
    if (selectedDevice && realTimeData.device_name !== selectedDevice && realTimeData.device_id !== selectedDevice) {
      return;
    }

    // 2. Kiểm tra bộ lọc Khoảng thời gian (nếu đang chọn)
    if (dateRange && dateRange.length === 2) {
      const fromTime = new Date(formatDateForAPI(dateRange[0], false)).getTime();
      const toTime = new Date(formatDateForAPI(dateRange[1], true)).getTime();
      const itemTime = new Date(realTimeData.ts).getTime();

      // Nếu dữ liệu realtime nằm ngoài khoảng đang lọc thì bỏ qua không hiển thị
      if (itemTime < fromTime || itemTime > toTime) {
        return;
      }
    }

    // Nếu thỏa mãn toàn bộ bộ lọc thì mới đưa vào danh sách hiển thị
    setData(prev => [realTimeData, ...prev.slice(0, 49)]);
  }
}, [realTimeData, selectedDevice, dateRange]); // Thêm selectedDevice và dateRange vào dependency array

  useEffect(() => {
    fetchDevices().then(res => setDevices(res.data)).catch(() => {});
  }, []);

  const handleExport = (format) => {
    const params = new URLSearchParams();
    if (selectedDevice) params.append('device_id', selectedDevice);
    if (dateRange && dateRange.length === 2) {
      params.append('from', formatDateForAPI(dateRange[0], false));
      params.append('to',   formatDateForAPI(dateRange[1], true));
    }
    const url = `${process.env.REACT_APP_API_URL || 'http://localhost:3000'}/api/export/${format}?${params.toString()}`;
    window.open(url, '_blank');
  };

  const columns = [
    {
      title: 'Thời gian',
      dataIndex: 'ts',
      key: 'ts',
      render: text => moment(text).format('HH:mm:ss DD/MM/YY'),
      sorter: (a, b) => moment(a.ts).unix() - moment(b.ts).unix(),
      defaultSortOrder: 'descend',
      sortDirections: ['descend', 'ascend'],
    },
    { title: 'Nhiệt độ', dataIndex: 'temperature', key: 'temp', render: v => v?.toFixed(1) + ' °C' },
    { title: 'Độ ẩm', dataIndex: 'humidity', key: 'hum', render: v => v?.toFixed(1) + ' %' },
    { title: 'Raw Temp', dataIndex: 'raw_temperature', key: 'raw_temp', render: v => v?.toFixed(1) + ' °C' },
    { title: 'Raw Hum', dataIndex: 'raw_humidity', key: 'raw_hum', render: v => v?.toFixed(1) + ' %' },
    {
      title: 'Hành động',
      render: (_, record) => <Button type="link" onClick={() => onViewDetail(record.id)}>Chi tiết</Button>
    }
  ];

  return (
    <div>
      <Space style={{ marginBottom: 16, flexWrap: 'wrap' }}>
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
        <Button icon={<DownloadOutlined />} onClick={() => handleExport('csv')}>
          Xuất CSV
        </Button>
        <Button icon={<DownloadOutlined />} onClick={() => handleExport('json')}>
          Xuất JSON
        </Button>
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
