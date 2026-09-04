// MPU6050 readings for Arduino Serial Plotter

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

Adafruit_MPU6050 mpu;

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!mpu.begin()) {
    Serial.println("MPU6050_Error: 0");
    while (1) {
      delay(10);
    }
  }

  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  delay(100);
}

void loop() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  Serial.print("Accel_X:");
  Serial.print(a.acceleration.x);

  Serial.print("\tAccel_Y:");
  Serial.print(a.acceleration.y);

  Serial.print("\tAccel_Z:");
  Serial.print(a.acceleration.z);

  Serial.print("\tGyro_X:");
  Serial.print(g.gyro.x);

  Serial.print("\tGyro_Y:");
  Serial.print(g.gyro.y);

  Serial.print("\tGyro_Z:");
  Serial.print(g.gyro.z);

  Serial.print("\tTemp:");
  Serial.println(temp.temperature);

  delay(20);  // approximately 50 Hz
}