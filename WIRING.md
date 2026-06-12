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
| Signal | GPIO |
|--------|------|
| RELAY  | 26   |
| BUZZ   | 27   |
| LED1   | 14   |
| LED2   | 13   |
| LED3   | 12   |

## PN532 SPI mode: SEL0=GND, SEL1=GND

## Flash
```
pio run --target upload
pio run --target uploadfs
# copy sd_card/www/index.html → SD /www/index.html
```

## Default login: admin / 12345678
