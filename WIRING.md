# InOut v0.3 — Wiring

## VSPI — PN532 readers (dedicated)
| Signal  | GPIO | Notes              |
|---------|------|--------------------|
| SCK     | 18   |                    |
| MISO    | 19   |                    |
| MOSI    | 23   |                    |
| CS IN   | 4    | PN532 #1 (READER1) |
| CS OUT  | 25   | PN532 #2 (READER0) |

## HSPI — SD card (dedicated)
| Signal  | GPIO | Notes |
|---------|------|-------|
| SCK     | 17   |       |
| MISO    | 16   |       |
| MOSI    | 33   |       |
| CS      | 5    |       |

## I2C — LCD 1602
| Signal | GPIO | Notes                      |
|--------|------|----------------------------|
| SDA    | 21   | I2C address: 0x27 or 0x3F  |
| SCL    | 22   |                            |

## Other
| Signal      | GPIO | Notes                                                       |
|-------------|------|--------------------------------------------------------------|
| RELAY       | 26   |                                                                |
| BUZZ        | 15   |                                                                |
| SERVER_LED  | 27   | lit while talking to the server (sync/heartbeat/proxy/OTA)   |
| READER_LED  | 14   | lit while an NFC reader is in use; blinks once at boot if both readers pass self-test |
| SD_LED      | 13   | lit while the SD card is being read or written               |
| CAM_LED     | 12   | lit while a photo capture is in progress                     |

## UART2 — ESP32-CAM (AI-Thinker)
| Signal        | Main ESP32 GPIO | CAM GPIO | Notes              |
|---------------|-----------------|----------|--------------------|
| TX (main→cam) | 32              | 3        | UART0 RX on CAM    |
| RX (main←cam) | 34              | 1        | UART0 TX on CAM    |
| GND           | GND             | GND      |                    |

> To flash the CAM: connect FTDI TX→GPIO3, RX→GPIO1, pull GPIO0 to GND
> on power-on, release after upload, then reset.

## PN532 SPI mode: SEL0=GND, SEL1=GND

## Flash
```
pio run --target upload
pio run --target uploadfs
# copy sd_card/www/index.html → SD /www/index.html
```

## Default login: admin / 12345678
