#pragma once

// Aphelion ESP32-S3 with an I2C OLED and an INMP441 whose L/R pin is grounded.
enum {
    I2C_SDA = 6, I2C_SCL = 7,
    MIC_SD = 41, MIC_SCK = 40, MIC_WS = 39,
};
