# FocusLamp 引脚映射表

## 总览

| 模块 | 信号名 | GPIO | M3引脚 | 功能说明 |
|------|--------|------|--------|---------|
| 电源管理 | PWR_CTRL | GPIO23 | M3-23 | 系统电源控制（MOSFET 开关） |
| 电源管理 | ESP_EN | GPIO45 | M3-45 | ESP32-P4 主控使能 |
| RGB LED | LED_DIN | GPIO6 | M3-24 | WS2812 LED 灯带数据输入 (RMT) |
| 环境光 | TEMT_OUT | GPIO21 | M3-33 | TEMT6000 环境光传感器模拟输出 (ADC) |
| 触摸 A | TTP_A | GPIO9 | M3-34 | 触摸点 A 输入 (TTP223) |
| 触摸 B | TTP_B | GPIO22 | M3-35 | 触摸点 B 输入 (TTP223) |
| 触摸 C | TTP_C | GPIO10 | M3-36 | 触摸点 C 输入 (TTP223) |
| 触摸 D | TTP_D | GPIO23 | M3-37 | 触摸点 D 输入 (TTP223) |
| LCD | LCD_BL | GPIO51 | M3-54 | LCD 背光 PWM 控制 |
| LCD | LCD_SCL | GPIO50 | M3-55 | LCD SPI 时钟线 |
| LCD | LCD_SDA | GPIO36 | M3-56 | LCD SPI 数据线 |
| LCD | LCD_DC | GPIO49 | M3-57 | LCD 数据/命令选择 |
| LCD | LCD_CS | GPIO34 | M3-58 | LCD SPI 片选 |
| I2S 音频 | AUD_LRC | GPIO33 | M3-59 | I2S 左右声道时钟 (WS) |
| I2S 音频 | AUD_BCLK | GPIO32 | M3-61 | I2S 位时钟 (BCK) |
| I2S 音频 | AUD_DIN | GPIO31 | M3-63 | I2S 数据输入 (DAC) |
| I2S 音频 | AUD_SD | GPIO30 | M3-65 | 音频放大器关断控制 |
| 舵机 1 | SERVO_TXD1 | GPIO8 | EM3 TX | 舵机 1 (EM3) UART 发送 |
| 舵机 1 | SERVO_RXD1 | GPIO2 | EM3 RX | 舵机 1 (EM3) UART 接收 |
| 舵机 1 | SERVO_OE1 | GPIO11 | EM3 OE | 舵机 1 (EM3) RS485 输出使能 |
| 舵机 2 | SERVO_TXD2 | GPIO3 | LX TX | 舵机 2 (LX) UART 发送 |
| 舵机 2 | SERVO_RXD2 | GPIO20 | LX RX | 舵机 2 (LX) UART 接收 |
| 舵机 2 | SERVO_OE2 | GPIO4 | LX OE | 舵机 2 (LX) RS485 输出使能 |
| 雷达 | RADAR_RX | GPIO7 | M3-25 | 雷达传感器 UART 接收 |
| 雷达 | RADAR_TX | GPIO1 | M3-26 | 雷达传感器 UART 发送 |
| 双板 UART2 | UART2_TX | GPIO54 | M3-42 | 双板通信发送 |
| 双板 UART2 | UART2_RX | GPIO53 | M3-44 | 双板通信接收 |

## 电源引脚

| 电源 | 电压 | M3引脚 |
|------|------|--------|
| 3.3V | 3.3V | M3-1, M3-3 |
| 5V | 5.0V | M3-41, M3-43 |
| GND | 0V | M3-39, M3-40, M3-79, M3-80 |

## 引脚功能分类

### 数字输出
| GPIO | 信号 | 初始电平 | 说明 |
|------|------|---------|------|
| GPIO23 | PWR_CTRL | 高 (1) | 系统电源使能 |
| GPIO45 | ESP_EN | 高 (1) | ESP32-P4 使能 |
| GPIO11 | SERVO_OE1 | 低 (0) | 舵机 1 (EM3) 输出使能 |
| GPIO4 | SERVO_OE2 | 低 (0) | 舵机 2 (LX) 输出使能 |
| GPIO34 | LCD_CS | 高 (1) | LCD 片选 |
| GPIO49 | LCD_DC | 低 (0) | LCD 数据/命令 |
| GPIO51 | LCD_BL | 高 (1) | LCD 背光 |
| GPIO30 | AUD_SD | 低 (0) | 音频关断 |
| GPIO6 | LED_DIN | - | WS2812 数据 (RMT) |

### 数字输入
| GPIO | 信号 | 说明 |
|------|------|------|
| GPIO9 | TTP_A | 触摸 A |
| GPIO22 | TTP_B | 触摸 B |
| GPIO10 | TTP_C | 触摸 C |
| GPIO23 | TTP_D | 触摸 D |
| GPIO21 | TEMT_OUT | 环境光 (ADC 输入) |

### UART 接口
| UART | TX | RX | 用途 |
|------|----|----|------|
| UART0 | GPIO43 (内置) | GPIO44 (内置) | 调试日志 (USB) |
| UART1 | GPIO8 (SERVO_TXD1) | GPIO2 (SERVO_RXD1) | 舵机 1 (EM3) |
| UART1 (alt) | GPIO3 (SERVO_TXD2) | GPIO20 (SERVO_RXD2) | 舵机 2 (LX) |
| UART2 | GPIO54 (UART2_TX) | GPIO53 (UART2_RX) | 双板通信 |
| UART (radar) | GPIO1 (RADAR_TX) | GPIO7 (RADAR_RX) | 雷达传感器 |

### SPI (LCD)
| 信号 | GPIO | 说明 |
|------|------|------|
| SCLK | GPIO50 | SPI 时钟 |
| MOSI | GPIO36 | SPI 数据 (主出从入) |
| CS | GPIO34 | 片选 |
| DC | GPIO49 | 数据/命令 (非 SPI 标准信号) |

### I2S (音频 DAC)
| 信号 | GPIO | 说明 |
|------|------|------|
| LRCK | GPIO33 | 帧时钟/左右声道选择 |
| BCLK | GPIO32 | 位时钟 |
| DIN | GPIO31 | 串行数据输入 |

## 注意

1. **GPIO0**: 未使用（原为雷达 RX），需避免启动时被拉低
2. **GPIO46**: ESP32-P4 保留引脚，不可作为普通 GPIO 使用
3. **GPIO43/44**: 默认用于 UART0 调试输出，不在 M3 接口引出
4. **M3 接口** 使用 2.54mm 双排排针，共计 80 个引脚 (40x2)