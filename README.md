# Aphelion

Aphelion 是 ESP32-S3 上的离线 Web 工具箱，也提供带 SH1106 OLED、SHT40 和 INMP441 的传感器终端固件。

## 构建

在 ESP-IDF 6.1.1 环境中执行。两个配置使用独立的构建目录，支持当前 2 MiB Flash 布局。

| 固件 | 构建命令 |
| --- | --- |
| 裸板工具箱 | `idf.py -B build-toolbox -D APHELION_TOOLBOX=ON build` |
| OLED 传感器终端 | `idf.py -D IDF_TARGET=esp32s3 build` |

裸板烧录与串口监视，按实际设备替换 `/dev/ttyACM0`。

```sh
idf.py -B build-toolbox -D APHELION_TOOLBOX=ON -p /dev/ttyACM0 flash monitor
```

恢复 OLED 固件可使用 `idf.py -p /dev/ttyACM0 flash monitor`。两个配置使用同一 NVS 布局，烧录时保留已有配网记录。

## 工具箱

1. 从 USB 串口启动日志读取 `Aphelion-XXXX` 热点名称和 WPA2 密码。手机或电脑连上热点后打开 [设备控制台](http://192.168.4.1/)。
2. 要通过局域网使用工具箱，请在 **Wi-Fi 接入**中选择路由器并填写密码。连接成功后，换回路由器的 Wi-Fi，并打开页面显示的局域网地址。设备会记住路由器，启动时自动重连；热点也继续可用。
3. 在 **Wi-Fi 分析**和 **BLE 扫描**中分别扫描 2.4 GHz 网络和附近广播，可随时停止扫描。控制台一次由单个浏览器操作。
4. **数字示波器**可立即显示演示波形。要验证真实采样，断电后用跳线连接 GPIO5 与 GPIO4。上电后将 PWM 设为 **100 Hz、50%**，再采集 GPIO4。512 点、20 kS/s 的波形应显示 25.6 ms 时间窗，约可见 2.5 个周期。
5. ADC 输入限 0–3.3 V，接外部信号时要共地。满幅 PWM 可能超过 ADC 可校准量程，网页会标出削顶。离开页面或控制连接失效时，固件停止采样并将 GPIO5 输出置低。

## Web 接口

`GET /api/capabilities` 返回工具列表、引脚和采样参数。`/ws` 建立控制连接，发送 `hello`、`status` 和 `scan` JSON 消息，并推送示波器二进制帧。浏览器每秒发一次文本 `ping`，五秒未收到心跳时释放控制。REST 命令 `POST /api/scan`、`POST /api/pwm` 和 `POST /api/scope` 通过查询参数传入操作，并使用 WebSocket 会话提供的 `X-Aphelion-Control` 令牌。

`GET /api/network` 返回路由器连接状态和局域网地址。配网页使用 `POST /api/wifi/scan`、`GET /api/wifi/networks` 和 `POST /api/wifi`，这些接口限设备热点侧调用。

波形帧的前 12 字节依次为版本号、标志位、采样数、序号、采样率，随后是无符号 16 位采样值。多字节字段使用小端序。标志位的低三位分别表示演示数据、已校准毫伏值和削顶。未经校准的帧携带 ADC 原始计数。

## 后续开发

Ping/DNS/HTTP、TCP/UDP 和指定设备的 BLE GATT 读取属于下一阶段。GPIO、PCNT、RMT 和 UART/I²C/SPI 调试随后加入，再接入 FFT、转换工具和 LittleFS 文件管理。
