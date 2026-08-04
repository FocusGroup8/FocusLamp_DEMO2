# FocusLamp

Smart focus lamp project built on ESP-IDF (ESP32-P4).

## Hardware Overview

- **MCU**: ESP32-P4
- **Display**: SPI LCD with backlight control
- **Audio**: I2S digital audio output
- **Lighting**: RGB LED strip (60 LEDs) + ambient light sensor
- **Touch**: 4-channel capacitive touch (A/B/C/D)
- **Sensors**: Radar (UART), ambient light (analog)
- **Actuators**: Dual servo bus (UART), mechanical arm
- **Communication**: Dual-board UART2 inter-connect

## Project Structure

```
FocusLamp/
├── CMakeLists.txt              # Root project file
├── sdkconfig.defaults          # Default SDK configuration
├── partitions.csv              # Flash partition table
├── components/
│   ├── common/                 # Common definitions (pin config, system config, types, error codes)
│   ├── event_bus/              # Event bus (pub/sub messaging)
│   └── protocol/               # UART protocol, command parser, status packets
└── main/                       # Application entry point
```

## Build

```bash
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```