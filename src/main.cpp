#include "DHT.h"
#include <Arduino.h>
#include <esp_task_wdt.h>

// ==========================================
// KONFIGURASI HARDWARE
// ==========================================
#define DHTTYPE DHT22
#define PIN_SENSOR 15
#define PIN_BUTTON 12
#define PIN_LED 2

DHT dht(PIN_SENSOR, DHTTYPE);

// Kunci Rahasia untuk Enkripsi XOR Sederhana
const char KUNCI_XOR = 'K';

// ==========================================
// KONFIGURASI IoT & EDGE AI
// ==========================================
#define WINDOW_SIZE 10
#define ANOMALY_THRESHOLD 5.0
#define WDT_TIMEOUT_S 10

// ==========================================
// KONTRAK BERSAMA: GLOBAL OBJECT HANDLES
// ==========================================
QueueHandle_t xSensorQueue;
QueueHandle_t xProcessedQueue;

// Mutex untuk Serial Monitor
// CATATAN PENTING: FreeRTOS Mutex menggunakan mekanisme Priority Inheritance
// secara bawaan. Jika task prioritas rendah (SerialTask, prio=1) sedang memegang
// xSerialMutex, kemudian task prioritas tinggi (AlertTask, prio=4) mencoba
// mengambilnya, maka prioritas SerialTask akan dinaikkan sementara ke prio=4.
// Ini MENCEGAH terjadinya Priority Inversion (inversi prioritas) dimana task
// prioritas menengah bisa mendahului task yang sedang memegang resource.
SemaphoreHandle_t xSerialMutex;

// Task Handles — SEMUA disimpan untuk monitoring stack masing-masing task
TaskHandle_t xAlertTaskHandle = NULL;
TaskHandle_t xSensorTaskHandle = NULL;
TaskHandle_t xCryptoTaskHandle = NULL;
TaskHandle_t xSerialTaskHandle = NULL;

// ==========================================
// VARIABEL WCET (Worst-Case Execution Time) — Mikrodetik
// ==========================================
volatile uint32_t ulWCET_Sensor = 0;
volatile uint32_t ulWCET_Crypto = 0;
volatile uint32_t ulWCET_Serial = 0;
volatile uint32_t ulWCET_Alert = 0;

// ==========================================
// VARIABEL ISR DEBOUNCE
// ==========================================
volatile uint32_t ulLastISRTime = 0;
#define DEBOUNCE_DELAY_US 250000

// ==========================================
// RUNTIME TRACER — Pengganti Tracealyzer (Gratis, Wokwi-Compatible)
// ==========================================
// Custom lightweight tracer yang merekam semua event RTOS ke dalam
// circular buffer di RAM. Event yang direkam:
//   - Task mulai/selesai eksekusi (context switch)
//   - Queue send/receive (komunikasi antar-task)
//   - Mutex take/give (sinkronisasi)
//   - ISR enter/exit (interrupt handling)
//   - Task Notification (deferred processing)
//   - Anomali Edge AI terdeteksi

enum TraceEventType : uint8_t {
  EVT_TASK_START = 0,
  EVT_TASK_END,
  EVT_QUEUE_SEND,
  EVT_QUEUE_SEND_FAIL,
  EVT_QUEUE_RECV,
  EVT_MUTEX_TAKE,
  EVT_MUTEX_GIVE,
  EVT_MUTEX_FAIL,
  EVT_ISR_ENTER,
  EVT_ISR_EXIT,
  EVT_NOTIFY_SEND,
  EVT_NOTIFY_RECV,
  EVT_ANOMALY,
  EVT_CRC_OK,
  EVT_CRC_FAIL,
  EVT_WDT_FEED,
  EVT_PREEMPTED
};

enum TraceTaskId : uint8_t {
  TID_SENSOR = 0,
  TID_CRYPTO = 1,
  TID_ALERT = 2,
  TID_SERIAL = 3,
  TID_ISR = 4,
  TID_SYSTEM = 5
};

const char *TRACE_TASK_NAMES[] = {"SensorTask", "CryptoTask", "AlertTask",
                                  "SerialTask", "button_ISR", "System"};

const char *TRACE_EVENT_NAMES[] = {
    "TASK_START",    "TASK_END",      "QUEUE_SEND",    "QUEUE_SEND_FAIL",
    "QUEUE_RECV",    "MUTEX_TAKE",    "MUTEX_GIVE",    "MUTEX_FAIL",
    "ISR_ENTER",     "ISR_EXIT",      "NOTIFY_SEND",   "NOTIFY_RECV",
    "ANOMALY_DET",   "CRC_OK",        "CRC_FAIL",      "WDT_FEED",
    "PREEMPTED"};

struct TraceEvent {
  uint32_t timestamp_us;
  uint8_t eventType;
  uint8_t taskId;
  uint8_t detail;
  uint8_t padding;
};

#define TRACE_BUFFER_SIZE 150
TraceEvent traceBuffer[TRACE_BUFFER_SIZE];
volatile uint16_t traceHead = 0;
volatile uint16_t traceCount = 0;
uint32_t traceStartTime = 0;

portMUX_TYPE traceMux = portMUX_INITIALIZER_UNLOCKED;

#define TRACE_DUMP_INTERVAL 5
#define TRACE_DUMP_SHOW_LAST 40

void traceLog(uint8_t eventType, uint8_t taskId, uint8_t detail) {
  portENTER_CRITICAL(&traceMux);
  TraceEvent *evt = &traceBuffer[traceHead];
  evt->timestamp_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
  evt->eventType = eventType;
  evt->taskId = taskId;
  evt->detail = detail;
  evt->padding = 0;
  traceHead = (traceHead + 1) % TRACE_BUFFER_SIZE;
  if (traceCount < TRACE_BUFFER_SIZE) traceCount++;
  portEXIT_CRITICAL(&traceMux);
}

void IRAM_ATTR traceLogFromISR(uint8_t eventType, uint8_t taskId,
                               uint8_t detail) {
  portENTER_CRITICAL_ISR(&traceMux);
  TraceEvent *evt = &traceBuffer[traceHead];
  evt->timestamp_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
  evt->eventType = eventType;
  evt->taskId = taskId;
  evt->detail = detail;
  evt->padding = 0;
  traceHead = (traceHead + 1) % TRACE_BUFFER_SIZE;
  if (traceCount < TRACE_BUFFER_SIZE) traceCount++;
  portEXIT_CRITICAL_ISR(&traceMux);
}

// ==========================================
// HELPER: GARIS PEMISAH SERIAL OUTPUT
// ==========================================
void printSeparator() {
  Serial.println("────────────────────────────────────────────────");
}

void printDoubleSeparator() {
  Serial.println("════════════════════════════════════════════════");
}

// ==========================================
// FUNGSI DUMP TRACE — FORMAT LIST (BUKAN TABEL)
// ==========================================
// Menampilkan N event terakhir sebagai daftar sederhana yang mudah dibaca.
// PENTING: Harus dipanggil saat sudah memegang xSerialMutex!
void traceDump() {
  uint16_t currentCount = traceCount;
  uint16_t showCount =
      (TRACE_DUMP_SHOW_LAST < currentCount) ? TRACE_DUMP_SHOW_LAST
                                            : currentCount;
  if (showCount == 0) {
    Serial.println("  (belum ada event yang terekam)");
    return;
  }

  uint16_t startIdx;
  if (currentCount >= TRACE_BUFFER_SIZE) {
    startIdx =
        (traceHead + TRACE_BUFFER_SIZE - showCount) % TRACE_BUFFER_SIZE;
  } else {
    startIdx = (currentCount > showCount) ? (currentCount - showCount) : 0;
  }

  float elapsed_s =
      (float)(esp_timer_get_time() - traceStartTime) / 1000000.0;

  Serial.println();
  printDoubleSeparator();
  Serial.println("  RUNTIME TRACE LOG");
  printSeparator();
  Serial.printf("  Total events : %d\n", currentCount);
  Serial.printf("  Ditampilkan  : %d terakhir\n", showCount);
  Serial.printf("  Uptime       : %.1f detik\n", elapsed_s);
  printSeparator();

  for (uint16_t i = 0; i < showCount; i++) {
    uint16_t idx = (startIdx + i) % TRACE_BUFFER_SIZE;
    TraceEvent *evt = &traceBuffer[idx];

    float ts_ms = (float)evt->timestamp_us / 1000.0;

    // Buat string detail berdasarkan tipe event
    char detailStr[32];
    switch (evt->eventType) {
    case EVT_QUEUE_SEND:
      snprintf(detailStr, sizeof(detailStr), "-> %s",
               evt->detail == 0 ? "xSensorQueue" : "xProcessedQueue");
      break;
    case EVT_QUEUE_SEND_FAIL:
      snprintf(detailStr, sizeof(detailStr), "-> %s (FULL!)",
               evt->detail == 0 ? "xSensorQueue" : "xProcessedQueue");
      break;
    case EVT_QUEUE_RECV:
      snprintf(detailStr, sizeof(detailStr), "<- %s",
               evt->detail == 0 ? "xSensorQueue" : "xProcessedQueue");
      break;
    case EVT_MUTEX_TAKE:
      snprintf(detailStr, sizeof(detailStr), "xSerialMutex LOCKED");
      break;
    case EVT_MUTEX_GIVE:
      snprintf(detailStr, sizeof(detailStr), "xSerialMutex RELEASED");
      break;
    case EVT_MUTEX_FAIL:
      snprintf(detailStr, sizeof(detailStr), "xSerialMutex TIMEOUT!");
      break;
    case EVT_ISR_ENTER:
      snprintf(detailStr, sizeof(detailStr), "GPIO%d FALLING", PIN_BUTTON);
      break;
    case EVT_ISR_EXIT:
      snprintf(detailStr, sizeof(detailStr), "dur < 10us");
      break;
    case EVT_NOTIFY_SEND:
      snprintf(detailStr, sizeof(detailStr), "-> AlertTask (wake)");
      break;
    case EVT_NOTIFY_RECV:
      snprintf(detailStr, sizeof(detailStr), "notification received");
      break;
    case EVT_ANOMALY:
      snprintf(detailStr, sizeof(detailStr), "suhu anomali!");
      break;
    case EVT_CRC_OK:
      snprintf(detailStr, sizeof(detailStr), "CRC 0x%02X OK", evt->detail);
      break;
    case EVT_CRC_FAIL:
      snprintf(detailStr, sizeof(detailStr), "CRC MISMATCH!");
      break;
    case EVT_WDT_FEED:
      snprintf(detailStr, sizeof(detailStr), "watchdog fed");
      break;
    case EVT_TASK_START:
      snprintf(detailStr, sizeof(detailStr), "prio=%d", evt->detail);
      break;
    case EVT_TASK_END:
      snprintf(detailStr, sizeof(detailStr), "done");
      break;
    case EVT_PREEMPTED:
      snprintf(detailStr, sizeof(detailStr), "by prio=%d", evt->detail);
      break;
    default:
      snprintf(detailStr, sizeof(detailStr), "?");
    }

    Serial.printf("  %10.3f ms  %-10s  %-16s  %s\n", ts_ms,
                  TRACE_TASK_NAMES[evt->taskId],
                  TRACE_EVENT_NAMES[evt->eventType], detailStr);
  }

  printDoubleSeparator();
  Serial.println();
}

// ==========================================
// RING BUFFER UNTUK I/O BUFFERING
// ==========================================
#define RING_BUFFER_SIZE 512
char ringBuffer[RING_BUFFER_SIZE];
volatile uint16_t rbHead = 0;
volatile uint16_t rbTail = 0;

void rbWrite(const char *str) {
  while (*str) {
    uint16_t nextHead = (rbHead + 1) % RING_BUFFER_SIZE;
    if (nextHead == rbTail) break;
    ringBuffer[rbHead] = *str++;
    rbHead = nextHead;
  }
}

void rbFlushToSerial() {
  while (rbTail != rbHead) {
    Serial.write(ringBuffer[rbTail]);
    rbTail = (rbTail + 1) % RING_BUFFER_SIZE;
  }
}

// ==========================================
// CRC-8 UNTUK DATA INTEGRITY CHECK
// ==========================================
uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x80)
        crc = (crc << 1) ^ 0x07;
      else
        crc <<= 1;
    }
  }
  return crc;
}

// ==========================================
// STRUKTUR DATA
// ==========================================
struct SensorData {
  float temperature;
  char ciphertext[16];
  uint8_t crc;
  bool anomaly;
};

// ==========================================
// VARIABEL EDGE AI: MOVING AVERAGE & ANOMALY DETECTION
// ==========================================
float tempWindow[WINDOW_SIZE];
int windowIndex = 0;
int windowCount = 0;

float calculateMovingAverage() {
  if (windowCount == 0) return 0.0;
  float sum = 0;
  int count = min(windowCount, WINDOW_SIZE);
  for (int i = 0; i < count; i++) {
    sum += tempWindow[i];
  }
  return sum / count;
}

float calculateVariance(float mean) {
  if (windowCount <= 1) return 0.0;
  float sumSq = 0;
  int count = min(windowCount, WINDOW_SIZE);
  for (int i = 0; i < count; i++) {
    float diff = tempWindow[i] - mean;
    sumSq += diff * diff;
  }
  return sumSq / count;
}

bool detectAnomaly(float newTemp) {
  tempWindow[windowIndex] = newTemp;
  windowIndex = (windowIndex + 1) % WINDOW_SIZE;
  if (windowCount < WINDOW_SIZE) windowCount++;

  if (windowCount < 3) return false;

  float mean = calculateMovingAverage();
  float deviation = abs(newTemp - mean);

  return (deviation > ANOMALY_THRESHOLD);
}

// ==========================================
// INTERRUPT SERVICE ROUTINE (ISR) — DENGAN DEBOUNCE + TRACE
// ==========================================
void IRAM_ATTR button_ISR() {
  uint32_t currentTime = (uint32_t)esp_timer_get_time();

  if ((currentTime - ulLastISRTime) < DEBOUNCE_DELAY_US) {
    return;
  }
  ulLastISRTime = currentTime;

  traceLogFromISR(EVT_ISR_ENTER, TID_ISR, PIN_BUTTON);

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  vTaskNotifyGiveFromISR(xAlertTaskHandle, &xHigherPriorityTaskWoken);

  traceLogFromISR(EVT_NOTIFY_SEND, TID_ISR, 0);
  traceLogFromISR(EVT_ISR_EXIT, TID_ISR, 0);

  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// ==========================================
// DEKLARASI FUNGSI TASK
// ==========================================
void vTaskSensorAcquisition(void *pvParameters);
void vTaskCryptoProcess(void *pvParameters);
void vTaskAlertSystem(void *pvParameters);
void vTaskSerialOutput(void *pvParameters);

// ==========================================
// INITIALIZATION (SETUP)
// ==========================================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  dht.begin();
  memset(tempWindow, 0, sizeof(tempWindow));

  traceStartTime = (uint32_t)esp_timer_get_time();

  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), button_ISR, FALLING);

  esp_task_wdt_init(WDT_TIMEOUT_S, true);

  xSensorQueue = xQueueCreate(5, sizeof(SensorData));
  xProcessedQueue = xQueueCreate(5, sizeof(SensorData));
  xSerialMutex = xSemaphoreCreateMutex();

  if (xSensorQueue != NULL && xProcessedQueue != NULL &&
      xSerialMutex != NULL) {

    xTaskCreatePinnedToCore(vTaskAlertSystem, "AlertTask", 4096, NULL, 4,
                            &xAlertTaskHandle, 1);
    xTaskCreatePinnedToCore(vTaskSensorAcquisition, "SensorTask", 4096, NULL,
                            3, &xSensorTaskHandle, 1);
    xTaskCreatePinnedToCore(vTaskCryptoProcess, "CryptoTask", 4096, NULL, 2,
                            &xCryptoTaskHandle, 1);
    xTaskCreatePinnedToCore(vTaskSerialOutput, "SerialTask", 4096, NULL, 1,
                            &xSerialTaskHandle, 1);

    Serial.println();
    printDoubleSeparator();
    Serial.println("  SECURE IoT EDGE NODE");
    Serial.println("  FreeRTOS + Runtime Tracer");
    printSeparator();
    Serial.printf("  Watchdog     : %d detik\n", WDT_TIMEOUT_S);
    Serial.printf("  Free Heap    : %d bytes\n", xPortGetFreeHeapSize());
    Serial.printf("  Trace buffer : %d events (%d bytes)\n",
                  TRACE_BUFFER_SIZE,
                  (int)(TRACE_BUFFER_SIZE * sizeof(TraceEvent)));
    Serial.printf("  Trace dump   : setiap %d iterasi\n",
                  TRACE_DUMP_INTERVAL);
    printDoubleSeparator();
    Serial.println();

    traceLog(EVT_TASK_START, TID_SYSTEM, 0);
  } else {
    Serial.println("[FATAL] Gagal membuat objek RTOS!");
  }
}

void loop() { vTaskDelay(portMAX_DELAY); }

// ==========================================
// REALISASI TASK
// ==========================================

// --- TASK 1: SENSOR ACQUISITION & EDGE AI ANOMALY DETECTION ---
void vTaskSensorAcquisition(void *pvParameters) {
  SensorData dataMentah;
  const TickType_t xPeriod = pdMS_TO_TICKS(2000);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  esp_task_wdt_add(NULL);

  for (;;) {
    traceLog(EVT_TASK_START, TID_SENSOR, 3);

    uint64_t start_time = esp_timer_get_time();

    float t = dht.readTemperature();

    if (isnan(t)) {
      dataMentah.temperature = 0.0;
      dataMentah.anomaly = false;
    } else {
      dataMentah.temperature = t;
      dataMentah.anomaly = detectAnomaly(t);

      if (dataMentah.anomaly) {
        traceLog(EVT_ANOMALY, TID_SENSOR, (uint8_t)t);
      }
    }

    memset(dataMentah.ciphertext, 0, sizeof(dataMentah.ciphertext));
    dataMentah.crc = 0;
    dataMentah.crc = crc8((uint8_t *)&dataMentah.temperature, sizeof(float));

    if (xQueueSend(xSensorQueue, &dataMentah, pdMS_TO_TICKS(100)) == pdTRUE) {
      traceLog(EVT_QUEUE_SEND, TID_SENSOR, 0);
    } else {
      traceLog(EVT_QUEUE_SEND_FAIL, TID_SENSOR, 0);
      if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        Serial.println("  [!] xSensorQueue penuh, data di-drop");
        xSemaphoreGive(xSerialMutex);
      }
    }

    uint64_t duration = esp_timer_get_time() - start_time;
    if (duration > ulWCET_Sensor) {
      ulWCET_Sensor = duration;
    }

    esp_task_wdt_reset();
    traceLog(EVT_WDT_FEED, TID_SENSOR, 0);
    traceLog(EVT_TASK_END, TID_SENSOR, 0);

    vTaskDelayUntil(&xLastWakeTime, xPeriod);
  }
}

// --- TASK 2: CRYPTO PROCESSING & DATA INTEGRITY ---
void vTaskCryptoProcess(void *pvParameters) {
  SensorData dataSistem;

  for (;;) {
    if (xQueueReceive(xSensorQueue, &dataSistem, portMAX_DELAY) == pdTRUE) {
      traceLog(EVT_QUEUE_RECV, TID_CRYPTO, 0);
      traceLog(EVT_TASK_START, TID_CRYPTO, 2);

      uint64_t start_time = esp_timer_get_time();

      // Verifikasi CRC-8
      uint8_t expectedCRC =
          crc8((uint8_t *)&dataSistem.temperature, sizeof(float));
      if (expectedCRC != dataSistem.crc) {
        traceLog(EVT_CRC_FAIL, TID_CRYPTO, dataSistem.crc);
        if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          Serial.println("  [!] CRC mismatch — data corrupt");
          xSemaphoreGive(xSerialMutex);
        }
        continue;
      }

      traceLog(EVT_CRC_OK, TID_CRYPTO, dataSistem.crc);

      // Enkripsi XOR
      char teksMentah[16];
      dtostrf(dataSistem.temperature, 4, 2, teksMentah);

      for (int i = 0; i < (int)strlen(teksMentah); i++) {
        dataSistem.ciphertext[i] = teksMentah[i] ^ KUNCI_XOR;
      }
      dataSistem.ciphertext[strlen(teksMentah)] = '\0';

      dataSistem.crc =
          crc8((uint8_t *)dataSistem.ciphertext, strlen(dataSistem.ciphertext));

      if (xQueueSend(xProcessedQueue, &dataSistem, pdMS_TO_TICKS(100)) ==
          pdTRUE) {
        traceLog(EVT_QUEUE_SEND, TID_CRYPTO, 1);
      } else {
        traceLog(EVT_QUEUE_SEND_FAIL, TID_CRYPTO, 1);
        if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          Serial.println("  [!] xProcessedQueue penuh, data di-drop");
          xSemaphoreGive(xSerialMutex);
        }
      }

      uint64_t duration = esp_timer_get_time() - start_time;
      if (duration > ulWCET_Crypto) {
        ulWCET_Crypto = duration;
      }

      traceLog(EVT_TASK_END, TID_CRYPTO, 0);
    }
  }
}

// --- TASK 3: ISR DEFERRED PROCESSING & PHYSICAL ALERT ---
void vTaskAlertSystem(void *pvParameters) {
  uint32_t ulNotificationValue;

  for (;;) {
    ulNotificationValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (ulNotificationValue > 0) {
      traceLog(EVT_NOTIFY_RECV, TID_ALERT, ulNotificationValue);
      traceLog(EVT_TASK_START, TID_ALERT, 4);

      uint64_t start_time = esp_timer_get_time();
      uint64_t active_duration = 0;

      if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        traceLog(EVT_MUTEX_TAKE, TID_ALERT, 0);

        Serial.println();
        printDoubleSeparator();
        Serial.println("  [!] PHYSICAL TAMPERING DETECTED");
        Serial.println("  [!] Deferred processing dari button_ISR()");
        Serial.println("  [!] LED alarm berkedip 6x ...");
        printDoubleSeparator();

        xSemaphoreGive(xSerialMutex);
        traceLog(EVT_MUTEX_GIVE, TID_ALERT, 0);
      } else {
        traceLog(EVT_MUTEX_FAIL, TID_ALERT, 0);
      }

      active_duration += (esp_timer_get_time() - start_time);

      for (int i = 0; i < 6; i++) {
        uint64_t led_on_start = esp_timer_get_time();
        digitalWrite(PIN_LED, HIGH);
        active_duration += (esp_timer_get_time() - led_on_start);

        vTaskDelay(pdMS_TO_TICKS(100));

        uint64_t led_off_start = esp_timer_get_time();
        digitalWrite(PIN_LED, LOW);
        active_duration += (esp_timer_get_time() - led_off_start);

        vTaskDelay(pdMS_TO_TICKS(100));
      }

      if (active_duration > ulWCET_Alert) {
        ulWCET_Alert = active_duration;
      }

      traceLog(EVT_TASK_END, TID_ALERT, 0);
    }
  }
}

// --- TASK 4: SERIAL OUTPUT, MEMORY, IoT, SCHEDULABILITY & TRACE DUMP ---
void vTaskSerialOutput(void *pvParameters) {
  SensorData dataSiapCetak;
  uint32_t packetCount = 0;

  for (;;) {
    if (xQueueReceive(xProcessedQueue, &dataSiapCetak, portMAX_DELAY) ==
        pdTRUE) {
      traceLog(EVT_QUEUE_RECV, TID_SERIAL, 1);

      if (xSemaphoreTake(xSerialMutex, portMAX_DELAY) == pdTRUE) {
        traceLog(EVT_MUTEX_TAKE, TID_SERIAL, 0);
        traceLog(EVT_TASK_START, TID_SERIAL, 1);

        uint64_t start_time = esp_timer_get_time();
        packetCount++;

        // ── DATA LOG ──────────────────────────────
        Serial.println();
        printDoubleSeparator();
        Serial.printf("  DATA LOG #%lu\n", (unsigned long)packetCount);
        printSeparator();

        Serial.printf("  Suhu (Plaintext)  : %.2f C\n",
                      dataSiapCetak.temperature);

        Serial.print("  Ciphertext (HEX)  : ");
        for (int i = 0; i < (int)strlen(dataSiapCetak.ciphertext); i++) {
          Serial.printf("%02X ", (unsigned char)dataSiapCetak.ciphertext[i]);
        }
        Serial.println();

        Serial.printf("  CRC-8 Integrity   : 0x%02X\n", dataSiapCetak.crc);

        if (dataSiapCetak.anomaly) {
          Serial.println("  Edge AI Status    : ANOMALI TERDETEKSI!");
        } else {
          Serial.println("  Edge AI Status    : Normal");
        }

        // ── IoT JSON PAYLOAD ──────────────────────
        printSeparator();
        Serial.println("  IoT PAYLOAD (MQTT/HTTP Ready)");
        printSeparator();

        char jsonPayload[256];
        snprintf(jsonPayload, sizeof(jsonPayload),
                 "  {\"node_id\":\"ESP32-EDGE-01\","
                 "\"temp\":%.2f,"
                 "\"anomaly\":%s,"
                 "\"crc\":\"0x%02X\","
                 "\"packet\":%lu,"
                 "\"uptime_ms\":%lu}",
                 dataSiapCetak.temperature,
                 dataSiapCetak.anomaly ? "true" : "false",
                 dataSiapCetak.crc, (unsigned long)packetCount,
                 (unsigned long)millis());

        // Tulis ke ring buffer lalu flush (demonstrasi I/O buffering)
        rbWrite(jsonPayload);
        rbWrite("\n");
        rbFlushToSerial();

        // ── MEMORY MONITOR ────────────────────────
        printSeparator();
        Serial.println("  MEMORY MONITOR");
        printSeparator();

        Serial.printf("  Heap  : free = %d bytes, min ever = %d bytes\n",
                      xPortGetFreeHeapSize(),
                      xPortGetMinimumEverFreeHeapSize());

        Serial.printf("  Stack : Sensor = %d w, Crypto = %d w\n",
                      uxTaskGetStackHighWaterMark(xSensorTaskHandle),
                      uxTaskGetStackHighWaterMark(xCryptoTaskHandle));
        Serial.printf("          Alert  = %d w, Serial = %d w\n",
                      uxTaskGetStackHighWaterMark(xAlertTaskHandle),
                      uxTaskGetStackHighWaterMark(xSerialTaskHandle));

        // ── SCHEDULABILITY ANALYSIS ───────────────
        uint64_t current_c_serial = esp_timer_get_time() - start_time;
        if (current_c_serial > ulWCET_Serial) {
          ulWCET_Serial = current_c_serial;
        }

        float c1 = (float)ulWCET_Sensor / 1000.0;
        float c2 = (float)ulWCET_Crypto / 1000.0;
        float c3 = (float)ulWCET_Serial / 1000.0;
        float c4 = (float)ulWCET_Alert / 1000.0;

        float t1 = 2000.0;
        float t2 = 2000.0;
        float t3 = 2000.0;
        float t4 = 5000.0;

        float u_total = (c1 / t1) + (c2 / t2) + (c3 / t3) + (c4 / t4);
        float u_lub = 4.0 * (pow(2.0, 1.0 / 4.0) - 1.0);

        printSeparator();
        Serial.println("  SCHEDULABILITY — RATE MONOTONIC SCHEDULING");
        printSeparator();

        Serial.println("  [A] LUB Test (Liu & Layland)");
        Serial.printf("      SensorTask : C1 = %.4f ms, T1 = %.0f ms\n",
                      c1, t1);
        Serial.printf("      CryptoTask : C2 = %.4f ms, T2 = %.0f ms\n",
                      c2, t2);
        Serial.printf("      SerialTask : C3 = %.4f ms, T3 = %.0f ms\n",
                      c3, t3);
        Serial.printf("      AlertTask  : C4 = %.4f ms, T4 = %.0f ms\n",
                      c4, t4);
        Serial.printf("      U total = %.6f (%.4f%%)\n", u_total,
                      u_total * 100.0);
        Serial.printf("      U LUB   = %.6f (%.4f%%)\n", u_lub,
                      u_lub * 100.0);

        if (u_total <= u_lub) {
          Serial.println(
              "      Status  = PASS (U <= LUB, dijamin schedulable)");
        } else if (u_total <= 1.0) {
          Serial.println(
              "      Status  = EXCEEDED (U > LUB, perlu RTA)");
        } else {
          Serial.println(
              "      Status  = FAIL (U > 100%, overload!)");
        }

        // ── RTA (RESPONSE TIME ANALYSIS) ──────────
        Serial.println();
        Serial.println("  [B] Response Time Analysis (RTA — Exact Test)");

        float C[4] = {c4, c1, c2, c3};
        float T[4] = {t4, t1, t2, t3};
        const char *names[4] = {"AlertTask ", "SensorTask", "CryptoTask",
                                "SerialTask"};
        float R[4];
        bool allSchedulable = true;

        for (int i = 0; i < 4; i++) {
          float r = C[i];
          float r_prev = 0;
          int iteration = 0;
          bool converged = false;

          while (r != r_prev && iteration < 100) {
            r_prev = r;
            r = C[i];

            for (int j = 0; j < i; j++) {
              r += ceil(r_prev / T[j]) * C[j];
            }

            if (r > T[i]) {
              break;
            }

            iteration++;
            if (r == r_prev) converged = true;
          }

          R[i] = r;

          const char *status;
          if (converged && r <= T[i]) {
            status = "PASS";
          } else {
            status = "FAIL";
            allSchedulable = false;
          }

          Serial.printf("      R(%s) = %.4f ms, D = %.0f ms  [%s]\n",
                        names[i], R[i], T[i], status);
        }

        if (allSchedulable) {
          Serial.println(
              "      Hasil RTA : PASS — semua task memenuhi deadline");
        } else {
          Serial.println(
              "      Hasil RTA : FAIL — ada task melewati deadline");
        }

        printDoubleSeparator();

        // ── RUNTIME TRACE DUMP (Setiap N iterasi) ─
        if (packetCount % TRACE_DUMP_INTERVAL == 0) {
          traceDump();
        }

        traceLog(EVT_TASK_END, TID_SERIAL, 0);
        traceLog(EVT_MUTEX_GIVE, TID_SERIAL, 0);

        xSemaphoreGive(xSerialMutex);
      }
    }
  }
}