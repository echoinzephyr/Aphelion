# Aphelion

Aphelion 是 ESP32-S3 上的离线 Web 工具箱，支持 SH1106 OLED、SHT40 和 INMP441。

## 构建

在 ESP-IDF 6.1.1 环境中构建和烧录。默认固件同时提供网页工具箱和 OLED，使用 2 MiB Flash 布局。命令中的 `-D APHELION_TOOLBOX=ON` 也会更新已有构建目录里的旧配置。按实际设备替换 `/dev/ttyACM0`。

```sh
idf.py -D APHELION_TOOLBOX=ON -p /dev/ttyACM0 flash monitor
```

烧录会保留已有 Wi‑Fi 配网记录。

## 工具箱

1. 从 USB 串口启动日志读取 `Aphelion-XXXX` 热点名称和 WPA2 密码。手机或电脑连上热点后打开 [设备控制台](http://192.168.4.1/)。
2. 要通过局域网使用工具箱，请在 **Wi-Fi 接入**中选择路由器并填写密码。连接成功后，换回路由器的 Wi-Fi，并打开页面显示的局域网地址。设备会记住路由器，启动时自动重连；热点也继续可用。
3. 在 **工具**中打开 **Wi-Fi 分析**或 **BLE 扫描**，分别扫描 2.4 GHz 网络和附近广播，可随时停止扫描。控制台一次由单个浏览器操作。
4. 打开 **工具 → 数字示波器**即可查看演示波形。要验证真实采样，断电后用跳线连接 GPIO5 与 GPIO4。上电后将 PWM 设为 **100 Hz、50%**，再采集 GPIO4。512 点、20 kS/s 的波形应显示 25.6 ms 时间窗，约可见 2.5 个周期。
5. ADC 输入限 0–3.3 V，接外部信号时要共地。满幅 PWM 可能超过 ADC 可校准量程，网页会标出削顶。离开页面或控制连接失效时，固件停止采样并将 GPIO5 输出置低。

### BLE Explorer

打开 **工具 → BLE 扫描**，扫描后点击设备名称。广播包和扫描响应都能查看原始 HEX 和解析字段。可连接的设备支持浏览 GATT 服务与特征值，按特征值提供的权限读取、写入和订阅通知或指示。

同一时间连接一个设备。断开后可重新扫描。会话日志记录这条连接上的读写、通知和状态，支持导出 JSON。设备配置保存在当前浏览器，载入后可以选中匹配的服务与特征值并填入 HEX 数据。

## OLED 时钟

连上有互联网访问的路由器后，设备通过 NTP 自动校时。OLED 用单色点阵显示设备时区的时间和日期（默认香港，UTC+8），未校时显示「未校时」。网页的 **传感器 → 切换 OLED 页面** 可在时钟、总览和传感器详情之间切换。控制台的 **系统设置 → 设备时间** 可保存 OLED 时区、查看 NTP 状态，或用手机和电脑的时间手动校时。功能开关也保存在设备上。`GET /api/time` 返回同步状态与本地时间。

## Web 接口

`GET /api/status` 返回传感器、网络、OLED 状态和实际检测到的存储容量。`GET /api/settings` 读取功能开关和时区，`PUT /api/settings` 保存变更，`POST /api/time` 可手动校时。修改请求带上 `X-Aphelion: settings`。`POST /api/button` 使用 `X-Aphelion: button` 请求头切换 OLED 页面。

`GET /api/capabilities` 返回工具列表、引脚和采样参数。`/ws` 建立控制连接，发送 `hello`、`status` 和 `scan` JSON 消息，并推送示波器二进制帧。浏览器每秒发一次文本 `ping`，五秒未收到心跳时释放控制。REST 命令 `POST /api/scan`、`POST /api/pwm` 和 `POST /api/scope` 通过查询参数传入操作，并使用 WebSocket 会话提供的 `X-Aphelion-Control` 令牌。屏幕可用时，`GET /api/display` 返回屏幕状态与传感器摘要，`POST /api/display` 使用同一令牌切换 OLED 页面。

`POST /api/ble` 使用同一控制令牌，通过查询参数提交 `connect`、`disconnect`、`read`、`write` 或 `subscribe`。写入请求正文是最多 512 字节的二进制值。`/ws` 的 `ble` 消息携带连接状态和发现结果，`ble_value` 消息携带操作结果及通知。WebUI 会标出发现或通知记录达到容量上限的情况。

`GET /api/network` 返回路由器连接状态和局域网地址。配网页使用 `POST /api/wifi/scan`、`GET /api/wifi/networks` 和 `POST /api/wifi`，这些接口限设备热点侧调用。

波形帧的前 12 字节依次为版本号、标志位、采样数、序号、采样率，随后是无符号 16 位采样值。多字节字段使用小端序。标志位的低三位分别表示演示数据、已校准毫伏值和削顶。未经校准的帧携带 ADC 原始计数。

## 后续开发

Ping/DNS/HTTP 和 TCP/UDP 属于下一阶段。GPIO、PCNT、RMT 和 UART/I²C/SPI 调试随后加入，再接入 FFT、转换工具和 LittleFS 文件管理。
