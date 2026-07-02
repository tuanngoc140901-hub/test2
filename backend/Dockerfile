# Sử dụng Node.js bản nhẹ Alpine
FROM node:18-alpine

# Tạo thư mục làm việc
WORKDIR /app

# Copy file package.json và package-lock.json (nếu có) trước để tận dụng cache
COPY package*.json ./

# Cài đặt dependencies (chỉ production)
RUN npm install --omit=dev

# Copy toàn bộ source code
COPY . .

# Backend chạy trên cổng 3000
EXPOSE 3000

# Khởi động server
CMD ["node", "server.js"]
