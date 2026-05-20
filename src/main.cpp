#include <Arduino.h>
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "hid_host.h"
#include "hid_usage_keyboard.h"
#include "hid_usage_mouse.h"

static const char *TAG = "PRESENTER";
QueueHandle_t hid_host_event_queue;
bool user_shutdown = false;

typedef struct {
    hid_host_device_handle_t hid_device_handle;
    hid_host_driver_event_t event;
    void *arg;
} hid_host_event_queue_t;

static const char *hid_proto_name_str[] = {"NONE", "KEYBOARD", "MOUSE"};

// --- Keyboard Report ---
static void hid_host_keyboard_report_callback(const uint8_t *const data, const int length) {
    if (length < 8) return;

    uint8_t modifier = data[0];
    static uint8_t prev_modifier = 0;

    if (modifier != prev_modifier) {
        if ((modifier & 0x02) && !(prev_modifier & 0x02))
            printf("  >>> [Shift-L] PRESSED <- Presenter-Taste\n");
        if (!(modifier & 0x02) && (prev_modifier & 0x02))
            printf("  >>> [Shift-L] RELEASED\n");
        if ((modifier & 0x20) && !(prev_modifier & 0x20))
            printf("  >>> [Shift-R] PRESSED\n");
        prev_modifier = modifier;
    }

    static uint8_t prev_keys[6] = {0};
    for (int i = 2; i < 8 && i < length; i++) {
        uint8_t k = data[i];
        if (k == 0) continue;

        bool found = false;
        for (int j = 0; j < 6; j++) {
            if (prev_keys[j] == k) { found = true; break; }
        }
        if (found) continue;

        printf("  Key 0x%02X: ", k);
        switch (k) {
            case 0x4B: printf("PAGE UP = Slide Backward\n"); break;
            case 0x4E: printf("PAGE DOWN = Slide Forward\n"); break;
            case 0x3E: printf("F5 = Start Slideshow\n"); break;
            case 0x29: printf("ESCAPE\n"); break;
            case 0x28: printf("ENTER\n"); break;
            case 0x2C: printf("SPACE\n"); break;
            case 0x2B: printf("TAB = Presenter-Taste\n"); break;
            case 0x4F: printf("ARROW RIGHT\n"); break;
            case 0x50: printf("ARROW LEFT\n"); break;
            case 0x51: printf("ARROW DOWN\n"); break;
            case 0x52: printf("ARROW UP\n"); break;
            default:   printf("(unknown)\n"); break;
        }
    }
    memcpy(prev_keys, &data[2], 6);
}

// --- Mouse Report ---
static void hid_host_mouse_report_callback(const uint8_t *const data, const int length) {
    printf("  Mouse/Consumer (%d B): ", length);
    for (int i = 0; i < length; i++) printf("%02X ", data[i]);
    printf("\n");

    int offset = 0;
    if (length >= 3 && data[0] != 0 && data[0] < 0x10) {
        printf("  Report ID: 0x%02X\n", data[0]);
        offset = 1;
    }

    if (length - offset >= 2) {
        uint16_t usage = data[offset] | (data[offset + 1] << 8);
        if (usage != 0) {
            switch (usage) {
                case 0x00E9: printf("  >>> VOLUME UP\n"); break;
                case 0x00EA: printf("  >>> VOLUME DOWN\n"); break;
                case 0x00E2: printf("  >>> MUTE\n"); break;
                default: printf("  >>> Consumer 0x%04X\n", usage); break;
            }
        }
    }
    fflush(stdout);
}

// --- Generic Report ---
static void hid_host_generic_report_callback(const uint8_t *const data, const int length) {
    printf("Generic (%d B): ", length);
    for (int i = 0; i < length; i++) printf("%02X ", data[i]);
    printf("\n");

    int offset = 0;
    if (length >= 3 && data[0] != 0 && data[0] < 0x10) {
        printf("  Report ID: 0x%02X\n", data[0]);
        offset = 1;
    }

    if (length - offset >= 1) {
        uint16_t usage = (length - offset >= 2) ? (data[offset] | (data[offset + 1] << 8)) : data[offset];
        if (usage != 0) {
            switch (usage) {
                case 0x00E9: printf("  >>> VOLUME UP\n"); break;
                case 0x00EA: printf("  >>> VOLUME DOWN\n"); break;
                case 0x00E2: printf("  >>> MUTE\n"); break;
                default: printf("  >>> Usage 0x%04X\n", usage); break;
            }
        }
    }
    fflush(stdout);
}

// --- Interface Callback ---
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                  const hid_host_interface_event_t event, void *arg) {
    uint8_t data[64] = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
            ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(
                hid_device_handle, data, 64, &data_length));

            printf("\n[%s] RAW: ", hid_proto_name_str[dev_params.proto]);
            for (size_t i = 0; i < data_length; i++) printf("%02X ", data[i]);
            printf("\n");

            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
                if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                    hid_host_keyboard_report_callback(data, data_length);
                } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
                    hid_host_mouse_report_callback(data, data_length);
                }
            } else {
                hid_host_generic_report_callback(data, data_length);
            }
            fflush(stdout);
            break;

        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HID Device '%s' DISCONNECTED", hid_proto_name_str[dev_params.proto]);
            ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
            break;

        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGI(TAG, "HID Device '%s' TRANSFER_ERROR", hid_proto_name_str[dev_params.proto]);
            break;

        default:
            break;
    }
}

// --- Device Event ---
void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                            const hid_host_driver_event_t event, void *arg) {
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback,
        .callback_arg = NULL
    };

    switch (event) {
        case HID_HOST_DRIVER_EVENT_CONNECTED:
            ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED", hid_proto_name_str[dev_params.proto]);

            ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
            // Don't set boot protocol - presenter needs report protocol for special keys
            hid_class_request_set_idle(hid_device_handle, 0, 0);
            ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
            printf("  Device started! Waiting for reports...\n");
            break;

        default:
            break;
    }
}

// --- USB Lib Task ---
static void usb_lib_task(void *arg) {
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (!user_shutdown) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB Event flags: ALL_FREE");
        }
    }

    vTaskDelay(10);
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

// --- HID Host Task ---
void hid_host_task(void *pvParameters) {
    hid_host_event_queue_t evt_queue;
    hid_host_event_queue = xQueueCreate(10, sizeof(hid_host_event_queue_t));

    while (!user_shutdown) {
        if (xQueueReceive(hid_host_event_queue, &evt_queue, pdMS_TO_TICKS(50))) {
            hid_host_device_event(evt_queue.hid_device_handle, evt_queue.event, evt_queue.arg);
        }
    }

    xQueueReset(hid_host_event_queue);
    vQueueDelete(hid_host_event_queue);
    vTaskDelete(NULL);
}

// --- HID Host Device Callback ---
void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                               const hid_host_driver_event_t event, void *arg) {
    const hid_host_event_queue_t evt_queue = {
        .hid_device_handle = hid_device_handle,
        .event = event,
        .arg = arg
    };
    xQueueSend(hid_host_event_queue, &evt_queue, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "Presenter HID Tester");

    printf("\n========================================\n");
    printf("  Presenter HID Tester - ESP32-S3\n");
    printf("========================================\n\n");
    printf("Erwartete Tasten:\n");
    printf("  Slide Forward  -> Page Down (0x4E)\n");
    printf("  Slide Backward -> Page Up (0x4B)\n");
    printf("  Presenter-Taste-> Shift-L (Modifier 0x02)\n");
    printf("  Volume Up      -> Consumer 0x00E9\n");
    printf("  Volume Down    -> Consumer 0x00EA\n\n");

    xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                            xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    ulTaskNotifyTake(false, 1000);

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    user_shutdown = false;
    xTaskCreate(&hid_host_task, "hid_task", 4 * 1024, NULL, 2, NULL);

    printf("USB Host gestartet. Stecke den Dongle ein...\n\n");
}

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    app_main();
}

void loop() {
    delay(100);
}
