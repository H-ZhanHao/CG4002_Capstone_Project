idf_component_register(
    SRCS
        "main.cpp"
        "node/ContinuousCounter.cpp"
        "node/ExerciseNode.cpp"
        "node/FallDetector.cpp"
        "node/Mpu6050.cpp"
        "node/NodePlatform.cpp"
        "node/NodeRadio.cpp"
        "node/NodeSession.cpp"
        "node/RepCounter.cpp"
        "node/WirelessStreamer.cpp"
    INCLUDE_DIRS "node"
    REQUIRES bt esp_driver_gpio esp_driver_i2c esp_driver_uart esp_event
             esp_netif esp_timer esp_wifi nvs_flash
)
