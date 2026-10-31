idf_component_register(
    SRCS
        "../../../../../hardware/node/ContinuousCounter.cpp"
        "../../../../../hardware/node/NodeSession.cpp"
        "../../../../../hardware/node/RepCounter.cpp"
        "src/GenericPeakDetector.cpp"
        "src/Mpu6050.cpp"
        "src/SensorHistory.cpp"
        "src/exercise_bridge.cpp"
        "src/hardware_bridge.cpp"
    INCLUDE_DIRS "include" "../../../../../hardware/node"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_timer freertos
)


