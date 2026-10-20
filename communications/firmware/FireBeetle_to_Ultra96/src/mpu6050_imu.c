/**
 * @file mpu6050_imu.c
 * @brief Scheduled 100 Hz capture from an MPU6050 on GPIO21/GPIO22.
 */

#include "mpu6050_imu.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "imu_source.h"

#define MPU6050_I2C_PORT       I2C_NUM_0
#define MPU6050_SDA_GPIO       21
#define MPU6050_SCL_GPIO       22
#define MPU6050_I2C_HZ         400000
#define MPU6050_ADDRESS        0x68
#define MPU6050_WHO_AM_I       0x75
#define MPU6050_PWR_MGMT_1     0x6B
#define MPU6050_CONFIG         0x1A
#define MPU6050_SMPLRT_DIV     0x19
#define MPU6050_GYRO_CONFIG    0x1B
#define MPU6050_ACCEL_CONFIG   0x1C
#define MPU6050_ACCEL_XOUT_H   0x3B
#define MPU6050_SAMPLE_MS      10
#define MPU6050_ARM_POLL_MS    10
#define MPU6050_TASK_STACK     3072
#define MPU6050_TASK_PRIORITY  4
#define MPU6050_IO_TIMEOUT_MS  20

static const char *TAG = "mpu6050";

typedef struct {
    QueueHandle_t output_queue;
    TaskHandle_t task_handle;
    uint8_t node_id;
    volatile bool paused;
    volatile bool armed;
    volatile bool running;
    int64_t scheduled_start_us;
    volatile uint32_t dropped_sample_count;
    volatile uint32_t read_error_count;
} mpu6050_source_t;

static mpu6050_source_t sources[7];
static portMUX_TYPE source_state_lock = portMUX_INITIALIZER_UNLOCKED;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t i2c_device;

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    uint8_t bytes[2] = {reg, value};
    return i2c_master_transmit(i2c_device, bytes, sizeof(bytes),
        MPU6050_IO_TIMEOUT_MS);
}

static esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(i2c_device, &reg, 1, data, length,
        MPU6050_IO_TIMEOUT_MS);
}

static int16_t signed_be(const uint8_t *bytes)
{
    return (int16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static esp_err_t initialize_sensor(void)
{
    if (i2c_bus == NULL) {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = MPU6050_I2C_PORT,
            .sda_io_num = MPU6050_SDA_GPIO,
            .scl_io_num = MPU6050_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus),
            TAG, "I2C configuration failed");
        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = MPU6050_ADDRESS,
            .scl_speed_hz = MPU6050_I2C_HZ,
        };
        esp_err_t add_result = i2c_master_bus_add_device(
            i2c_bus, &device_config, &i2c_device);
        if (add_result != ESP_OK) {
            (void)i2c_del_master_bus(i2c_bus);
            i2c_bus = NULL;
            return add_result;
        }
    }

    uint8_t identity = 0;
    ESP_RETURN_ON_ERROR(read_registers(MPU6050_WHO_AM_I, &identity, 1),
        TAG, "MPU6050 did not answer on address 0x68");
    if (identity != 0x68) {
        ESP_LOGE(TAG, "Unexpected WHO_AM_I value 0x%02X", identity);
        return ESP_ERR_NOT_FOUND;
    }

    /* PLL clock, DLPF=3, 1 kHz/(1+9)=100 Hz, +/-4 g, +/-500 deg/s. */
    ESP_RETURN_ON_ERROR(write_register(MPU6050_PWR_MGMT_1, 0x01), TAG,
        "Could not wake MPU6050");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(write_register(MPU6050_CONFIG, 0x03), TAG,
        "Could not configure DLPF");
    ESP_RETURN_ON_ERROR(write_register(MPU6050_SMPLRT_DIV, 9), TAG,
        "Could not configure 100 Hz rate");
    ESP_RETURN_ON_ERROR(write_register(MPU6050_GYRO_CONFIG, 0x08), TAG,
        "Could not configure gyroscope");
    ESP_RETURN_ON_ERROR(write_register(MPU6050_ACCEL_CONFIG, 0x08), TAG,
        "Could not configure accelerometer");
    return ESP_OK;
}

static bool queue_sample_drop_oldest(QueueHandle_t queue,
    const imu_sample_t *sample)
{
    if (xQueueSend(queue, sample, 0) == pdPASS) return false;
    imu_sample_t discarded;
    if (xQueueReceive(queue, &discarded, 0) == pdPASS)
        (void)xQueueSend(queue, sample, 0);
    return true;
}

static void mpu6050_task(void *argument)
{
    mpu6050_source_t *source = argument;
    uint32_t sequence = 0;
    TickType_t next_wake = xTaskGetTickCount();
    ESP_LOGI(TAG, "Node %u MPU6050 ready at 100 Hz; waiting for START",
        source->node_id);

    while (true) {
        bool armed, running, paused;
        int64_t scheduled_start_us;
        portENTER_CRITICAL(&source_state_lock);
        armed = source->armed;
        running = source->running;
        paused = source->paused;
        scheduled_start_us = source->scheduled_start_us;
        portEXIT_CRITICAL(&source_state_lock);

        if (armed && !running) {
            int64_t remaining_us = scheduled_start_us - esp_timer_get_time();
            if (remaining_us > 2000) {
                vTaskDelay(pdMS_TO_TICKS((remaining_us - 1500) / 1000));
                continue;
            }
            if (remaining_us > 0) esp_rom_delay_us((uint32_t)remaining_us);
            portENTER_CRITICAL(&source_state_lock);
            if (source->armed && !source->running &&
                source->scheduled_start_us == scheduled_start_us) {
                source->armed = false;
                source->running = true;
                sequence = 0;
                next_wake = xTaskGetTickCount();
                running = true;
            }
            portEXIT_CRITICAL(&source_state_lock);
        }

        if (!running || paused) {
            next_wake = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(MPU6050_ARM_POLL_MS));
            continue;
        }

        uint8_t raw[14];
        int64_t captured_at_us = esp_timer_get_time();
        esp_err_t result = read_registers(MPU6050_ACCEL_XOUT_H, raw, sizeof(raw));
        if (result == ESP_OK) {
            imu_sample_t sample = {
                .node_id = source->node_id,
                .sequence = sequence,
                .timestamp_us = (uint64_t)captured_at_us,
                .ax = signed_be(&raw[0]), .ay = signed_be(&raw[2]),
                .az = signed_be(&raw[4]), .gx = signed_be(&raw[8]),
                .gy = signed_be(&raw[10]), .gz = signed_be(&raw[12]),
                .battery_mv = 0,
                .status = 0,
            };
            if (queue_sample_drop_oldest(source->output_queue, &sample))
                source->dropped_sample_count++;
            if ((sequence % 100) == 0)
                ESP_LOGI(TAG, "Live: node=%u seq=%lu a=[%d,%d,%d] g=[%d,%d,%d]",
                    source->node_id, (unsigned long)sequence, sample.ax, sample.ay,
                    sample.az, sample.gx, sample.gy, sample.gz);
            sequence++;
        } else {
            source->read_error_count++;
            if (source->read_error_count == 1 || source->read_error_count % 100 == 0)
                ESP_LOGE(TAG, "MPU6050 read failed (%lu): %s",
                    (unsigned long)source->read_error_count, esp_err_to_name(result));
        }
        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(MPU6050_SAMPLE_MS));
    }
}

esp_err_t mpu6050_imu_start(QueueHandle_t queue, uint8_t node_id)
{
    if (queue == NULL || node_id < 1 || node_id > 6) return ESP_ERR_INVALID_ARG;
    mpu6050_source_t *source = &sources[node_id];
    if (source->task_handle != NULL) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(initialize_sensor(), TAG, "MPU6050 initialization failed");
    source->output_queue = queue;
    source->node_id = node_id;
    BaseType_t created = xTaskCreate(mpu6050_task, "mpu6050", MPU6050_TASK_STACK,
        source, MPU6050_TASK_PRIORITY, &source->task_handle);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mpu6050_imu_set_paused(uint8_t node_id, bool paused)
{
    if (node_id < 1 || node_id > 6 || sources[node_id].task_handle == NULL)
        return ESP_ERR_INVALID_STATE;
    sources[node_id].paused = paused;
    return ESP_OK;
}

esp_err_t mpu6050_imu_schedule_start(uint8_t node_id, int64_t start_us)
{
    if (node_id < 1 || node_id > 6 || start_us <= esp_timer_get_time())
        return ESP_ERR_INVALID_ARG;
    mpu6050_source_t *source = &sources[node_id];
    if (source->task_handle == NULL) return ESP_ERR_INVALID_STATE;
    xQueueReset(source->output_queue);
    portENTER_CRITICAL(&source_state_lock);
    source->scheduled_start_us = start_us;
    source->armed = true;
    source->running = false;
    source->paused = false;
    portEXIT_CRITICAL(&source_state_lock);
    return ESP_OK;
}

esp_err_t mpu6050_imu_stop(uint8_t node_id)
{
    if (node_id < 1 || node_id > 6 || sources[node_id].task_handle == NULL)
        return ESP_ERR_INVALID_STATE;
    mpu6050_source_t *source = &sources[node_id];
    portENTER_CRITICAL(&source_state_lock);
    source->armed = false;
    source->running = false;
    source->paused = false;
    portEXIT_CRITICAL(&source_state_lock);
    xQueueReset(source->output_queue);
    return ESP_OK;
}

uint32_t mpu6050_imu_get_dropped_count(void)
{
    uint32_t total = 0;
    for (uint8_t node_id = 1; node_id <= 6; node_id++)
        total += sources[node_id].dropped_sample_count;
    return total;
}
