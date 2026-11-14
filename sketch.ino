#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// --- CẤU HÌNH PHẦN CỨNG ---
#define DHTPIN 15
#define DHTTYPE DHT22
#define PIN_MIST 18   // Máy phun sương (PWM) - Đèn Xanh dương
#define PIN_PUMP 19   // Máy bơm (Manual) - Đèn Xanh lá
#define PIN_FAN  21   // Quạt (Manual) - Đèn Vàng

// --- CẤU HÌNH WIFI & MQTT ---
const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_server = "broker.hivemq.com"; // Dùng MQTT Broker miễn phí
const int mqtt_port = 1883;

// Đổi chữ "nhom1" thành tên của bạn để không bị nhiễu dữ liệu
const char* topic_sensor = "vuon_cong_nghiep/nhom1/data";
const char* topic_ctrl_pump = "vuon_cong_nghiep/nhom1/control/pump";
const char* topic_ctrl_fan  = "vuon_cong_nghiep/nhom1/control/fan";

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

// --- KHAI BÁO CẤU TRÚC DỮ LIỆU & RTOS HANDLES ---
struct SensorData {
  float temperature;
  float humidity;
};

QueueHandle_t dataQueue;   // Hàng đợi truyền data lên MQTT
QueueHandle_t dataAlert;   // Hàng đợi báo động độ ẩm
SemaphoreHandle_t serialMutex; // Khóa bảo vệ tránh kẹt cổng Serial

// --- HÀM NHẬN LỆNH ĐIỀU KHIỂN TỪ WEB ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
    Serial.printf("[MQTT Nhận] Kênh: %s | Lệnh: %s\n", topic, message.c_str());
    xSemaphoreGive(serialMutex);
  }

  if (String(topic) == topic_ctrl_pump) {
    digitalWrite(PIN_PUMP, (message == "ON") ? HIGH : LOW);
  } else if (String(topic) == topic_ctrl_fan) {
    digitalWrite(PIN_FAN, (message == "ON") ? HIGH : LOW);
  }
}

// ==========================================================
// TASK 1: ĐỌC DỮ LIỆU CẢM BIẾN (CHẠY TRÊN CORE 1)
// ==========================================================
void Task_ReadData(void *pvParameters) {
  SensorData currentData;
  for (;;) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(h) && !isnan(t)) {
      currentData.temperature = t;
      currentData.humidity = h;

      if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
        Serial.printf("[Core %d - ReadData] Nhiệt độ: %.1f C | Độ ẩm: %.1f %%\n", xPortGetCoreID(), t, h);
        xSemaphoreGive(serialMutex);
      }

      // Đẩy dữ liệu vào hàng đợi (ghi đè nếu đầy để lấy số liệu mới nhất)
      xQueueOverwrite(dataQueue, &currentData);
      xQueueOverwrite(dataAlert, &h);
    }
    vTaskDelay(pdMS_TO_TICKS(2000)); // Đọc cảm biến mỗi 2 giây
  }
}

// ==========================================================
// TASK 2: CẢNH BÁO & BẬT MÁY PHUN SƯƠNG (CHẠY TRÊN CORE 1)
// ==========================================================
void Task_Alert(void *pvParameters) {
  float receivedHum = 0;
  for (;;) {
    if (xQueueReceive(dataAlert, &receivedHum, portMAX_DELAY) == pdPASS) {
      int pwmDuty = 0;
      int durationSec = 0;

      // Thuật toán kiểm tra ngưỡng độ ẩm
      if (receivedHum >= 35.0 && receivedHum < 40.0) {
        pwmDuty = 64;   // 25% công suất
        durationSec = 10;
      } else if (receivedHum >= 30.0 && receivedHum < 35.0) {
        pwmDuty = 128;  // 50% công suất
        durationSec = 20;
      } else if (receivedHum < 30.0) {
        pwmDuty = 255;  // 100% công suất
        durationSec = 30;
      }

      if (pwmDuty > 0) {
        if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
          Serial.printf(">> CẢNH BÁO: Độ ẩm %.1f%%! Bật phun sương PWM=%d trong %d giây.\n", receivedHum, pwmDuty, durationSec);
          xSemaphoreGive(serialMutex);
        }
        
        analogWrite(PIN_MIST, pwmDuty);
        vTaskDelay(pdMS_TO_TICKS(durationSec * 1000)); 
        analogWrite(PIN_MIST, 0); 
        xQueueReset(dataAlert); // Xóa hàng đợi để đọc mẫu mới sau khi phun xong
      } else {
        analogWrite(PIN_MIST, 0);
      }
    }
  }
}

// ==========================================================
// TASK 3: KẾT NỐI WIFI & ĐẨY DỮ LIỆU MQTT (CHẠY TRÊN CORE 0)
// ==========================================================
void Task_SendData(void *pvParameters) {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  
  if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
    Serial.println("[SendData] Da ket noi WiFi!");
    xSemaphoreGive(serialMutex);
  }

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  SensorData dataToSend;

  for (;;) {
    if (!client.connected()) {
      String clientId = "ESP_Vuon_" + String(random(0xffff), HEX);
      if (client.connect(clientId.c_str())) {
        client.subscribe(topic_ctrl_pump);
        client.subscribe(topic_ctrl_fan);
      } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }
    client.loop();

    if (xQueueReceive(dataQueue, &dataToSend, pdMS_TO_TICKS(100)) == pdPASS) {
      StaticJsonDocument<128> doc;
      doc["temp"] = dataToSend.temperature;
      doc["hum"] = dataToSend.humidity;
      doc["mist"] = (analogRead(PIN_MIST) > 0) ? "ON" : "OFF";

      char jsonBuffer[128];
      serializeJson(doc, jsonBuffer);
      client.publish(topic_sensor, jsonBuffer);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_MIST, OUTPUT);
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_FAN, OUTPUT);
  dht.begin();

  dataQueue = xQueueCreate(1, sizeof(SensorData));
  dataAlert = xQueueCreate(1, sizeof(float));
  serialMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(Task_SendData, "SendData", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(Task_ReadData, "ReadData", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_Alert,    "Alert",    4096, NULL, 3, NULL, 1);
}

void loop() { }