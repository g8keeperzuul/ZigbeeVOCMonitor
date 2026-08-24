# Architecture

Diagrams are Mermaid, which GitHub and VS Code render inline.

## 1. Components

Who calls whom. The application layer is deliberately thin: `main.c` owns
lifecycle and the Zigbee callbacks, and everything device-specific sits behind a
small module.

```mermaid
flowchart TB
    subgraph NET["Zigbee network"]
        Z2M["Zigbee2MQTT coordinator<br/>+ external converter"]
    end

    subgraph APP["Application - src/"]
        MAIN["main.c<br/>app_main, esp_zb_task, sensor_task<br/>signal handler, action handler"]
        ZBS["zb_sensor.c<br/>sensor endpoints, reporting,<br/>attribute publishing, self-test"]
        SENSOR["sensor.c<br/>SEN55 lifecycle,<br/>fixed-point to floats"]
        LEDC["led.c<br/>on / off"]
        DISP["display.c<br/>refresh policy, layout,<br/>2x headline renderer"]
    end

    subgraph VEND["components/sen5x - vendored BSD-3"]
        DRV["sen5x_i2c.c<br/>sensirion_i2c.c<br/>sensirion_common.c"]
        HAL["sensirion_i2c_hal.c<br/>our ESP-IDF port"]
    end

    subgraph VEND2["components/epaper - vendored Waveshare"]
        EPD["EPD_2in13_V4.c<br/>GUI_Paint.c<br/>font24 / font12"]
        EHAL["DEV_Config.c<br/>our ESP-IDF port"]
    end

    subgraph MC["Managed components"]
        ZB["esp-zigbee-lib<br/>esp-zboss-lib"]
        STRIP["led_strip"]
    end

    subgraph HW["Hardware"]
        SEN["SEN55<br/>I2C 0x69, SDA 6 / SCL 7"]
        LED["WS2812<br/>GPIO 8"]
        EINK["2.13in e-Paper V4<br/>SSD1680, 250x122<br/>SPI2: MOSI 23 / SCK 22"]
        RADIO["on-chip 802.15.4 radio"]
    end

    MAIN --> ZBS
    MAIN --> SENSOR
    MAIN --> LEDC
    MAIN --> DISP
    MAIN --> ZB
    ZBS --> ZB
    SENSOR --> DRV
    DRV --> HAL
    HAL -->|i2c_master| SEN
    DISP --> EPD
    EPD --> EHAL
    EHAL -->|spi_master| EINK
    LEDC --> STRIP
    STRIP -->|RMT| LED
    ZB --> RADIO
    RADIO <-->|802.15.4| Z2M
```

## 2. Tasks

Four threads of control. The split exists because the ZBOSS main loop never
returns, and both I2C transfers and e-Paper refreshes block.

```mermaid
flowchart LR
    subgraph T1["main task - exits after app_main"]
        A["nvs_flash_init<br/>led_init<br/>esp_zb_platform_config"]
    end

    subgraph T2["zigbee_main task, prio 5"]
        B["build endpoints<br/>esp_zb_device_register<br/>esp_zb_start"]
        C["esp_zb_stack_main_loop<br/>never returns"]
        D["signal handler<br/>action handler"]
    end

    subgraph T3["sen55 task, prio 4"]
        E["sen55_init"]
        F["every 10 s:<br/>read, scale, publish"]
    end

    subgraph T4["epaper task, prio 3"]
        G["panel_start<br/>Init, Clear, placeholder"]
        H["on new reading:<br/>gate, compare, refresh"]
    end

    A -->|xTaskCreate| B
    B --> C
    C -.->|callbacks| D
    B -->|xTaskCreate| E
    E --> F
    F -.->|esp_zb_lock_acquire| C
    B -->|xTaskCreate| G
    G --> H
    F -.->|xQueueOverwrite mailbox| H
```

The lock matters: `zb_sensor_publish()` runs on the sensor task and touches the
attribute store, which belongs to the stack. Callbacks in `D` are already inside
the stack's context and must **not** take the lock.

The e-Paper sits at the bottom of the priority order because a refresh blocks for
about a second, and it is fed through a one-deep `xQueueOverwrite` mailbox rather
than called directly — so the sensor task never waits on the panel, and a slow
refresh costs at most a dropped intermediate sample, never a delayed one.

## 3. Startup and commissioning

```mermaid
sequenceDiagram
    autonumber
    participant M as app_main
    participant ZT as zigbee_main
    participant ZB as ZBOSS
    participant ST as sen55 task
    participant S as SEN55
    participant C as Coordinator

    M->>M: nvs_flash_init
    M->>M: led_init, WS2812 on GPIO 8
    M->>ZB: esp_zb_platform_config, native radio
    M->>ZT: xTaskCreate zigbee_main

    ZT->>ZB: esp_zb_init, Router, max 10 children
    ZT->>ZT: light_endpoint_add, ep 10
    ZT->>ZT: zb_sensor_endpoints_add, ep 11..16
    ZT->>ZB: esp_zb_device_register
    ZT->>ZB: esp_zb_core_action_handler_register
    ZT->>ZB: esp_zb_start, autostart false
    ZT->>ZT: zb_sensor_selftest, logs 8 attributes
    ZT->>ST: xTaskCreate sen55
    ZT->>ZT: display_start, xTaskCreate epaper
    ZT->>ZB: esp_zb_stack_main_loop

    par Sensor brings itself up
        ST->>S: device reset, product name, serial
        ST->>S: sen5x_start_measurement
    and Stack finishes starting
        ZB-->>ZT: SKIP_STARTUP
        ZT->>ZB: BDB initialisation
        alt no stored credentials
            ZB-->>ZT: DEVICE_FIRST_START
            ZT->>ZB: BDB network steering
            ZB->>C: join request
            C-->>ZB: join accepted
            ZB-->>ZT: BDB_SIGNAL_STEERING ok
        else credentials in zb_storage
            ZB-->>ZT: DEVICE_REBOOT
        end
        ZT->>ZT: configure_reporting_once
    end

    Note over C: first join triggers the interview:<br/>Active_EP_req then Simple_Desc_req per endpoint
```

`configure_reporting_once()` is deliberately driven from the network-up signals.
Calling it straight after `esp_zb_start()` fails, because that call only queues
startup — ZBOSS has not built its reporting table yet.

## 4. Commissioning states

```mermaid
stateDiagram-v2
    [*] --> Initialising: esp_zb_start

    Initialising --> Steering: DEVICE_FIRST_START<br/>no credentials
    Initialising --> Joined: DEVICE_REBOOT<br/>credentials restored

    Steering --> Joined: STEERING ok
    Steering --> Retrying: STEERING failed

    Retrying --> Steering: after 5 s
    Retrying --> GaveUp: 100 attempts

    Joined --> Steering: ZDO_SIGNAL_LEAVE<br/>removed by coordinator

    state Joined {
        [*] --> ReportingConfigured: configure_reporting_once
        ReportingConfigured --> Publishing
        Publishing --> Publishing: every 10 s
    }

    GaveUp --> [*]: reset the board
```

## 5. Publishing a sample

```mermaid
sequenceDiagram
    autonumber
    participant ST as sen55 task
    participant S as SEN55
    participant ZBS as zb_sensor
    participant ZB as ZBOSS
    participant C as Coordinator

    loop every 10 s
        ST->>S: sen5x_read_data_ready
        alt no new sample
            S-->>ST: not ready
            Note over ST: skip this round
        else
            S-->>ST: ready
            ST->>S: sen5x_read_measured_values
            S-->>ST: fixed point, 0xFFFF / 0x7FFF for unknown
            ST->>ST: scale per channel, set pm_valid,<br/>rht_valid, voc_valid, nox_valid
            ST->>ZBS: zb_sensor_publish

            ZBS->>ZB: esp_zb_lock_acquire, 1 s timeout
            ZBS->>ZB: set_attribute_val, temperature + humidity
            ZBS->>ZB: set_attribute_val, PM2.5
            ZBS->>ZB: set_attribute_val, 5 Analog Inputs
            Note over ZBS: warming-up channels are skipped,<br/>so the last good value stands
            ZBS->>ZB: esp_zb_lock_release

            ZB-->>C: Report Attributes<br/>on delta, or at the 300 s ceiling
            ST->>ST: display_publish, see section 7
        end
    end
```

Scale factors and sentinels live entirely in `sensor.c`; the Zigbee side only
ever sees floats plus validity flags.

## 6. Turning the LED on and off

```mermaid
sequenceDiagram
    autonumber
    participant C as Coordinator
    participant ZB as ZBOSS
    participant AH as zb_action_handler
    participant L as led.c

    C->>ZB: On, Off or Toggle<br/>ep 10, cluster 0x0006
    ZB->>ZB: update the OnOff attribute
    ZB->>AH: SET_ATTR_VALUE_CB_ID
    AH->>AH: check ep 10 and cluster 0x0006
    AH->>L: led_set
    L->>L: led_strip_set_pixel + refresh
    ZB-->>C: Report Attributes, OnOff
```

All three commands arrive as a write of the same `OnOff` attribute, so one
handler covers them. The stack has already updated the store by then — the
handler only makes the hardware agree.

## 7. Refreshing the display

```mermaid
sequenceDiagram
    autonumber
    participant ST as sen55 task
    participant MB as mailbox
    participant DT as epaper task
    participant P as e-Paper V4

    ST->>MB: display_publish, xQueueOverwrite
    Note over MB: one deep -- a new sample<br/>replaces an unread one

    DT->>MB: xQueueReceive, 1 s timeout
    MB-->>DT: latest reading

    alt less than 60 s since last refresh
        Note over DT: skip, the once-a-minute cap
    else
        DT->>DT: format_screen into screen_t
        alt strings identical to what is drawn
            Note over DT: skip -- the pixels<br/>would not change
        else changed, or 24 h have passed
            DT->>DT: render into the 4000 byte buffer
            DT->>P: Init_Fast
            DT->>P: Display_Fast, ~1 s
            DT->>P: Sleep
            Note over P: asleep until the next update;<br/>re-init required on wake
        end
    end
```

The deadband compares formatted strings rather than floats, so it answers "would
the screen look different" exactly. In still air the panel refreshes far less
often than once a minute.

## 8. Channel to endpoint to MQTT

```mermaid
flowchart LR
    subgraph SEN55
        A1["temperature"]
        A2["humidity"]
        A3["PM2.5"]
        A4["VOC index"]
        A5["NOx index"]
        A6["PM1.0"]
        A7["PM4.0"]
        A8["PM10"]
    end

    subgraph EP["Zigbee endpoints"]
        E11["ep 11<br/>0x0402 Temperature<br/>0x0405 Humidity<br/>0x042A PM2.5"]
        E12["ep 12 - Analog Input"]
        E13["ep 13 - Analog Input"]
        E14["ep 14 - Analog Input"]
        E15["ep 15 - Analog Input"]
        E16["ep 16 - Analog Input"]
    end

    subgraph MQTT["MQTT payload keys"]
        M["temperature<br/>humidity<br/>pm25<br/>voc_index<br/>nox_index<br/>pm1<br/>pm4<br/>pm10"]
    end

    A1 --> E11
    A2 --> E11
    A3 --> E11
    A4 --> E12
    A5 --> E13
    A6 --> E14
    A7 --> E15
    A8 --> E16

    E11 -->|standard clusters| M
    E12 -->|converter| M
    E13 -->|converter| M
    E14 -->|converter| M
    E15 -->|converter| M
    E16 -->|converter| M
```

Endpoint 11's three channels have standard ZCL clusters and would be discovered
generically. The other five are bare Analog Input clusters carrying nothing that
says what they measure — the endpoint number is the only discriminator, which is
why the external converter maps endpoint to property name and why that map has
to track [`src/zb_sensor.h`](../src/zb_sensor.h).
