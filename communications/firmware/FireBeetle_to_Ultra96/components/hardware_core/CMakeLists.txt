idf_component_register(
    SRCS
        "../../../../../hardware/node/ContinuousCounter.cpp"
        "../../../../../hardware/node/NodeSession.cpp"
        "../../../../../hardware/node/RepCounter.cpp"
        "../../../../../hardware/node/FallDetector.cpp"
        "src/GenericPeakDetector.cpp"
        "src/Mpu6050.cpp"
        "src/SensorHistory.cpp"
        "src/FeedbackController.cpp"
        "src/FeedbackDiagnostic.cpp"
        "src/feedback_bridge.cpp"
        "src/exercise_bridge.cpp"
        "src/hardware_bridge.cpp"
        "src/fall_bridge.cpp"
    INCLUDE_DIRS "include" "../../../../../hardware/node"
    REQUIRES esp_driver_uart esp_driver_gpio esp_driver_i2c esp_timer freertos
)


