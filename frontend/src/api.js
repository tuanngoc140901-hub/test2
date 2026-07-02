import axios from 'axios';

const API_URL = process.env.REACT_APP_API_URL || 'http://localhost:3000';

export const fetchMeasurements = (params) => axios.get(`${API_URL}/api/measurements`, { params });
export const fetchMeasurement = (id) => axios.get(`${API_URL}/api/measurements/${id}`);
export const updateMeasurementNote = (id, note) => axios.patch(`${API_URL}/api/measurements/${id}`, { note });
export const fetchDevices = () => axios.get(`${API_URL}/api/devices`);
