import React, { useEffect, useState } from 'react';
import { Layout, Menu } from 'antd';
import { BarChartOutlined, DatabaseOutlined } from '@ant-design/icons';
import Dashboard from './components/Dashboard';
import MeasurementDetail from './components/MeasurementDetail';
import io from 'socket.io-client';

const { Header, Content, Sider } = Layout;
const socket = io(process.env.REACT_APP_WS_URL || 'http://localhost:3000');

function App() {
  const [currentPage, setCurrentPage] = useState('dashboard');
  const [selectedId, setSelectedId] = useState(null);
  const [realTimeData, setRealTimeData] = useState(null);

  useEffect(() => {
    socket.on('new_measurement', (data) => {
      setRealTimeData(data);
    });
    return () => socket.disconnect();
  }, []);

  const navigateToDetail = (id) => {
    setSelectedId(id);
    setCurrentPage('detail');
  };

  const renderContent = () => {
    if (currentPage === 'dashboard') {
      return <Dashboard onViewDetail={navigateToDetail} realTimeData={realTimeData} />;
    } else if (currentPage === 'detail' && selectedId) {
      return <MeasurementDetail id={selectedId} onBack={() => setCurrentPage('dashboard')} />;
    }
  };

  return (
    <Layout style={{ minHeight: '100vh' }}>
      <Sider collapsible>
        <div className="logo" style={{ color: 'white', textAlign: 'center', padding: 16 }}>E-Nose</div>
        <Menu theme="dark" defaultSelectedKeys={['dashboard']} onClick={({ key }) => setCurrentPage(key)}>
          <Menu.Item key="dashboard" icon={<BarChartOutlined />}>Dashboard</Menu.Item>
          <Menu.Item key="detail" disabled icon={<DatabaseOutlined />}>Chi tiết</Menu.Item>
        </Menu>
      </Sider>
      <Layout>
        <Header style={{ background: '#fff', padding: 0, textAlign: 'center', fontWeight: 'bold' }}>
          Hệ thống giám sát mùi E-Nose
        </Header>
        <Content style={{ margin: 16, padding: 24, background: '#fff' }}>
          {renderContent()}
        </Content>
      </Layout>
    </Layout>
  );
}

export default App;
