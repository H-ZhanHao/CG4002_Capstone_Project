# Keep the active communications sources explicit; ESP-NOW is no longer built.
idf_component_register(
    SRCS
        "application.c"
        "device_events.c"
        "feedback_delivery.c"
        "hub_router.c"
        "main.c"
        "mock_imu.c"
        "mpu6050_imu.c"
        "physical_pipeline.c"
        "protocol.c"
        "sensor_history.c"
        "tcp_client.c"
        "wifi_manager.c"
    REQUIRES mbedtls hardware_core esp_driver_i2c
)
