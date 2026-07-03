
# VXL / E-nose

This repository contains the VXL (E-nose) project: ESP32 firmware, backend and frontend code used for data collection, MQTT communication and a small dashboard.

Overview
- `esp32-firmware/`: ESP32 project (main firmware, drivers, build output).
- `backend/`: Node.js backend server and database initialization.
- `frontend/`: Web dashboard frontend.

Quick start
1. Configure Wi‑Fi and MQTT broker
	- Edit Wi-Fi and MQTT settings in `esp32-firmware/main/wifi_mqtt.c`.

2. Build & flash firmware (ESP-IDF)
	- Open a terminal, source your ESP-IDF environment, then:
	  ```bash
	  cd esp32-firmware
	  idf.py build
	  idf.py -p <PORT> flash
	  ```

3. Run backend
	- From `backend/` follow that folder's README (install deps and start server).

Notes
- Build artifacts are ignored via `.gitignore` (the `build/` directory). Do not commit generated binaries.
- Some files (e.g., `esp32-firmware/mlp.py` and `mlp_weights.h`) were removed from the repo to keep source clean.

How to update README
- This file was updated to include project overview and build/push steps.

If you want, I can also:
- Add more detailed build instructions for the backend or frontend.
- Add contribution and license sections.

Contact
- If you need specific changes, tell me which section to expand.


