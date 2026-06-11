#include "DHT.h"
#include <Arduino.h>


// Config untuk Sensor DHT22
#define DHTTYPE DHT22
#define PIN_SENSOR 15

DHT dht(PIN_SENSOR, DHTTYPE);

// Definisikan PIN untuk Komponen Lain
#define PIN_BUTTON 12
#define PIN_LED 2

// Kunci Rahasia untuk Enkripsi XOR Sederhana (Anggota 2)
const char KUNCI_XOR = 'K';

// ==========================================
// KONTRAK BERSAMA: GLOBAL OBJECT HANDLES
// ==========================================
QueueHandle_t xSensorQueue;
QueueHandle_t xProcessedQueue; // Jalur data dari Crypto ke Serial Output
SemaphoreHandle_t xSerialMutex;
TaskHandle_t xAlertTaskHandle = NULL;

// Variabel Pengukuran Worst-Case Execution Time (WCET) dalam Mikrodetik
volatile uint32_t ulWCET_Sensor = 0;
volatile uint32_t ulWCET_Crypto = 0;
volatile uint32_t ulWCET_Serial = 0;
volatile uint32_t ulWCET_Alert = 0;

// ==========================================
// STRUKTUR DATA
// ==========================================
struct SensorData {
  float temperature;
  char ciphertext[16];
};

// ==========================================
// TUGAS ANGGOTA 3: INTERRUPT SERVICE ROUTINE (ISR)
// ==========================================
void IRAM_ATTR button_ISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  // Kirim notifikasi kilat untuk membangunkan Alert Task
  vTaskNotifyGiveFromISR(xAlertTaskHandle, &xHigherPriorityTaskWoken);

  // Minta RTOS melakukan switch context jika Alert Task prioritasnya lebih
  // tinggi
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

  // Inisialisasi Hardware
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  dht.begin();

  // Atur Interupsi Tombol (Anggota 3)
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), button_ISR, FALLING);

  // Membuat Antrean / Queues
  xSensorQueue = xQueueCreate(5, sizeof(SensorData));
  xProcessedQueue = xQueueCreate(5, sizeof(SensorData));

  // Membuat Gembok / Mutex untuk Serial Monitor
  xSerialMutex = xSemaphoreCreateMutex();

  if (xSensorQueue != NULL && xProcessedQueue != NULL && xSerialMutex != NULL) {

    // PETA PRIORITAS TASK (PREEMPTIVE PRIORITY-BASED)
    xTaskCreatePinnedToCore(vTaskAlertSystem, "AlertTask", 2048, NULL, 4,
                            &xAlertTaskHandle, 1); // Paling Tinggi
    xTaskCreatePinnedToCore(vTaskSensorAcquisition, "SensorTask", 2048, NULL, 3,
                            NULL, 1); // Tinggi
    xTaskCreatePinnedToCore(vTaskCryptoProcess, "CryptoTask", 2048, NULL, 2,
                            NULL, 1); // Sedang
    xTaskCreatePinnedToCore(vTaskSerialOutput, "SerialTask", 2048, NULL, 1,
                            NULL, 1); // Rendah

    Serial.println("[SYSTEM] FreeRTOS Scheduler Dimulai...");
  } else {
    Serial.println("[ERROR] Gagal membuat objek RTOS!");
  }
}

void loop() { vTaskDelay(portMAX_DELAY); }

// ==========================================
// REALISASI TASK MASING-MASING ANGGOTA
// ==========================================

// --- TUGAS ANGGOTA 1: TASK MANAGEMENT & SCHEDULING ---
void vTaskSensorAcquisition(void *pvParameters) {
  SensorData dataMentah;
  const TickType_t xDelay2000ms = pdMS_TO_TICKS(2000);

  for (;;) {
    uint64_t start_time = esp_timer_get_time();

    float t = dht.readTemperature();

    if (isnan(t)) {
      dataMentah.temperature = 0.0;
    } else {
      dataMentah.temperature = t;
    }

    memset(dataMentah.ciphertext, 0, sizeof(dataMentah.ciphertext));

    // Kirim ke Crypto Task via xSensorQueue
    xQueueSend(xSensorQueue, &dataMentah, 0);

    uint64_t duration = esp_timer_get_time() - start_time;
    if (duration > ulWCET_Sensor) {
      ulWCET_Sensor = duration;
    }

    vTaskDelay(xDelay2000ms);
  }
}

// --- TUGAS ANGGOTA 2: INTER-TASK SYNCHRONIZATION & DATA ENCRYPTION ---
void vTaskCryptoProcess(void *pvParameters) {
  SensorData dataSistem;

  for (;;) {
    // 1. Ambil data mentah dari xSensorQueue (Aman & Thread-safe) [cite: 71]
    if (xQueueReceive(xSensorQueue, &dataSistem, portMAX_DELAY) == pdTRUE) {
      uint64_t start_time = esp_timer_get_time();

      // 2. Simulasi Kriptografi Ringan: Mengonversi float suhu ke string lalu
      // di-XOR
      char teksMentah[16];
      dtostrf(dataSistem.temperature, 4, 2, teksMentah); // Misal "25.50"

      // Proses Enkripsi Obfuscation per karakter
      for (int i = 0; i < strlen(teksMentah); i++) {
        dataSistem.ciphertext[i] = teksMentah[i] ^ KUNCI_XOR;
      }
      dataSistem.ciphertext[strlen(teksMentah)] = '\0'; // Penutup string

      // 3. Oper data terenkripsi ke xProcessedQueue untuk dicetak Task 4
      xQueueSend(xProcessedQueue, &dataSistem, 0);

      uint64_t duration = esp_timer_get_time() - start_time;
      if (duration > ulWCET_Crypto) {
        ulWCET_Crypto = duration;
      }
    }
  }
}

// --- TUGAS ANGGOTA 3: ISR & DEFERRED PROCESSING ---
void vTaskAlertSystem(void *pvParameters) {
  uint32_t ulNotificationValue;

  for (;;) {
    // 1. Task tidur pulas sampai dibangunkan oleh ISR tombol [cite: 71]
    ulNotificationValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (ulNotificationValue > 0) {
      uint64_t start_time = esp_timer_get_time();
      uint64_t active_duration = 0;

      // 2. Ambil gembok Mutex sebelum mencetak agar pesan tidak tabrakan [cite:
      // 71]
      if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        Serial.println("\n================================================");
        Serial.println("[WARNING] PHYSICAL TAMPERING DETECTED! ALARM ACTIVE!");
        Serial.println("================================================");
        xSemaphoreGive(xSerialMutex);
      }

      active_duration += (esp_timer_get_time() - start_time);

      // 3. Nyalakan Lampu Alarm Fisik (LED Merah Berkedip Cepat)
      for (int i = 0; i < 6; i++) {
        uint64_t led_on_start = esp_timer_get_time();
        digitalWrite(PIN_LED, HIGH);
        active_duration += (esp_timer_get_time() - led_on_start);

        vTaskDelay(pdMS_TO_TICKS(100)); // Passive delay (yield CPU)

        uint64_t led_off_start = esp_timer_get_time();
        digitalWrite(PIN_LED, LOW);
        active_duration += (esp_timer_get_time() - led_off_start);

        vTaskDelay(pdMS_TO_TICKS(100)); // Passive delay (yield CPU)
      }

      if (active_duration > ulWCET_Alert) {
        ulWCET_Alert = active_duration;
      }
    }
  }
}

// --- TUGAS ANGGOTA 4: MEMORY, I/O MANAGEMENT & LOG ---
void vTaskSerialOutput(void *pvParameters) {
  SensorData dataSiapCetak;

  for (;;) {
    // 1. Ambil data terenkripsi dari xProcessedQueue (Synchronized)
    if (xQueueReceive(xProcessedQueue, &dataSiapCetak, portMAX_DELAY) ==
        pdTRUE) {
      // 2. Ambil kunci Mutex untuk mengamankan fungsi cetak UART [cite: 71]
      if (xSemaphoreTake(xSerialMutex, portMAX_DELAY) == pdTRUE) {
        uint64_t start_time = esp_timer_get_time();

        Serial.print("[LOG] Plaintext Suhu: ");
        Serial.print(dataSiapCetak.temperature);
        Serial.print(" C | Ciphertext (Enkripsi): ");

        // Mencetak karakter enkripsi (mungkin berupa simbol aneh karena hasil
        // XOR)
        for (int i = 0; i < strlen(dataSiapCetak.ciphertext); i++) {
          Serial.print((unsigned char)dataSiapCetak.ciphertext[i], HEX);
          Serial.print(" ");
        }
        Serial.println();

        // 3. BONUS MANDAT: Monitor penggunaan sisa RAM (Stack) per Task [cite:
        // 71]
        Serial.print("[MONITOR STACK] Sisa Memory Ruang Kerja Task ini: ");
        Serial.print(uxTaskGetStackHighWaterMark(NULL));
        Serial.println(" bytes");
        Serial.println("------------------------------------------------");

        // --- UJI LEAST UPPER BOUND (LUB) ---
        // Hitung durasi eksekusi aktif Task 3 saat ini
        uint64_t current_c3 = esp_timer_get_time() - start_time;
        if (current_c3 > ulWCET_Serial) {
          ulWCET_Serial = current_c3;
        }

        // Ubah waktu eksekusi (WCET) ke milidetik
        float c1 = (float)ulWCET_Sensor / 1000.0;
        float c2 = (float)ulWCET_Crypto / 1000.0;
        float c3 = (float)ulWCET_Serial / 1000.0;
        float c4 = (float)ulWCET_Alert / 1000.0;

        float t1 = 2000.0; // Periode Task 1 (ms)
        float t2 = 2000.0; // Periode Task 2 (ms)
        float t3 = 2000.0; // Periode Task 3 (ms)
        float t4 = 5000.0; // Waktu antar-kedatangan minimum Task 4 (ms)

        float u_total = (c1 / t1) + (c2 / t2) + (c3 / t3) + (c4 / t4);
        float u_lub = 4.0 * (pow(2.0, 1.0 / 4.0) - 1.0); // ~0.75683

        Serial.println("================================================");
        Serial.println("[UJI LEAST UPPER BOUND (LUB) - SCHEDULABILITY]");
        Serial.printf("Task 1 (Sensor) WCET : C1 = %.4f ms\n", c1);
        Serial.printf("Task 2 (Crypto) WCET : C2 = %.4f ms\n", c2);
        Serial.printf("Task 3 (Serial) WCET : C3 = %.4f ms\n", c3);
        Serial.printf("Task 4 (Alert) WCET  : C4 = %.4f ms\n", c4);
        Serial.printf("Total Utilitas CPU (U)  : %.6f (%.4f%%)\n", u_total,
                      u_total * 100.0);
        Serial.printf("Batas LUB (n=4)         : %.6f (%.4f%%)\n", u_lub,
                      u_lub * 100.0);

        if (u_total <= u_lub) {
          Serial.println("Status Penjadwalan      : [PASS] Dijamin Schedulable "
                         "(U <= LUB)");
        } else if (u_total <= 1.0) {
          Serial.println("Status Penjadwalan      : [EXCEEDED] Melebihi LUB, "
                         "tapi mungkin schedulable");
        } else {
          Serial.println("Status Penjadwalan      : [FAIL] Overload (>100%), "
                         "tidak schedulable!");
        }
        Serial.println("================================================");

        xSemaphoreGive(xSerialMutex); // Kembalikan gembok
      }
    }
  }
}