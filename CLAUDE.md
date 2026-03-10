# Fireplace Display — ESP32-P4 Project

## Hardware
- **Board**: Waveshare ESP32-P4-86-Panel-ETH-2RO
- **MCU**: ESP32-P4 (RISC-V)
- **Display**: 720×720 round LCD, ST7703 controller, MIPI DSI interface
- **PSRAM**: 32 MB at 200 MHz (required for frame buffer)

## Key Pin Assignments
| Function       | GPIO        |
|----------------|-------------|
| LCD Backlight  | GPIO_NUM_26 |
| LCD Reset      | GPIO_NUM_27 |
| Touch I2C SDA  | GPIO_NUM_7  |
| Touch I2C SCL  | GPIO_NUM_8  |
| Touch RST      | GPIO_NUM_23 |
| Touch INT      | NC          |

## Touch Controller
- **Chip**: GT911 (Goodix capacitive, I2C address 0x5D)
- **Driver component**: `espressif/esp_lcd_touch_gt911`
- **I2C frequency**: 400 kHz on I2C_NUM_0
- **API**: `touch_init(callback)` in `main/touch.c` — polls every 10 ms, calls callback with (x, y)

## Display Interface
- **Protocol**: MIPI DSI, 2 lanes, 480 Mbps lane bit rate
- **Controller**: ST7703 (NOT ST7701 — confirmed via ESPHome waveshare.py)
- **Pixel format**: RGB565 little-endian
- **Resolution**: 720×720
- **DPI clock**: 38 MHz
- **MIPI PHY power**: on-chip LDO channel 3 at 2500 mV

### DPI Video Timing
```
hsync_back_porch = 50, hsync_pulse_width = 20, hsync_front_porch = 50
vsync_back_porch = 20, vsync_pulse_width = 4,  vsync_front_porch = 20
```

## Architecture
The display driver uses two separate ESP-IDF panel handles:
1. **DBI IO** (`esp_lcd_new_panel_io_dbi`) — command channel for sending ST7703 init registers
2. **DPI panel** (`esp_lcd_new_panel_dpi`) — pixel video stream, takes `dsi_bus` directly (no IO handle)

The `esp_lcd_st7701` component is **not used** — ST7703 init is sent manually via `esp_lcd_panel_io_tx_param`.

## Image Rendering
- Frame buffer lives in PSRAM, obtained via `esp_lcd_dpi_panel_get_frame_buffer`
- **Must use CPU `memcpy`** from flash to PSRAM — DMA2D cannot read flash-mapped memory
- `esp_lcd_panel_draw_bitmap` uses DMA2D internally and will silently fail for flash sources

## Embedded Image
- File: `main/fireplace.bin` — 1,036,800 bytes (720×720 × 2 bytes, RGB565 LE)
- Embedded via CMakeLists `EMBED_FILES "fireplace.bin"`
- Accessed via linker symbols: `_binary_fireplace_bin_start` / `_binary_fireplace_bin_end`

### Converting PNG → fireplace.bin (Python/Pillow)
```python
from PIL import Image
import struct

img = Image.open("source.png").convert("RGB").resize((720, 720), Image.LANCZOS)
with open("main/fireplace.bin", "wb") as f:
    for r, g, b in img.getdata():
        pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        f.write(struct.pack("<H", pixel))
```

## Build Configuration

### sdkconfig.defaults
```
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_IDF_EXPERIMENTAL_FEATURES=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

### partitions.csv
The default 1 MB factory partition is too small — the binary with embedded image is ~1.27 MB.
Factory partition is set to 3 MB (0x300000).

## ESP-IDF Version
- **v5.5.3**
- Compiler: `riscv32-esp-elf-gcc` from `~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/`
- IDF path: `~/.espressif/v5.5.3/esp-idf/`

## Build & Flash
```sh
idf.py build
idf.py flash monitor
```

## Debugging Tips
- **Color bar test**: `esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_BAR_VERTICAL)` — hardware test bypassing frame buffer, useful to confirm DSI timing and panel init are correct before debugging image issues.
- Check `esp_lcd_new_panel_dpi` signature — it takes `(dsi_bus, &dpi_cfg, &panel)`, NOT an IO handle.
- `esp_lcd_new_dpi_panel` does not exist; the correct name is `esp_lcd_new_panel_dpi`.
