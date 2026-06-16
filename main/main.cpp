#include <Arduino.h>
#include "DHT.h"
#include "trcRecorder.h"  // Trace Recorder untuk Tracealyzer

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

// ==========================================
// STRUKTUR DATA
// ==========================================
struct SensorData
{
    float temperature;
    char ciphertext[16];
};

// ==========================================
// TUGAS ANGGOTA 3: INTERRUPT SERVICE ROUTINE (ISR)
// ==========================================
void IRAM_ATTR button_ISR()
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Kirim notifikasi kilat untuk membangunkan Alert Task
    vTaskNotifyGiveFromISR(xAlertTaskHandle, &xHigherPriorityTaskWoken);

    // Minta RTOS melakukan switch context jika Alert Task prioritasnya lebih tinggi
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
void setup()
{
    Serial.begin(115200);
    delay(1000);  // Tunggu Serial siap
    
    // Mulai Tracealyzer Recording
    vTraceEnable(TRC_START);

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

    if (xSensorQueue != NULL && xProcessedQueue != NULL && xSerialMutex != NULL)
    {

        // PETA PRIORITAS TASK (PREEMPTIVE PRIORITY-BASED)
        xTaskCreatePinnedToCore(vTaskAlertSystem, "AlertTask", 2048, NULL, 4, &xAlertTaskHandle, 1); // Paling Tinggi
        xTaskCreatePinnedToCore(vTaskSensorAcquisition, "SensorTask", 2048, NULL, 3, NULL, 1);       // Tinggi
        xTaskCreatePinnedToCore(vTaskCryptoProcess, "CryptoTask", 2048, NULL, 2, NULL, 1);           // Sedang
        xTaskCreatePinnedToCore(vTaskSerialOutput, "SerialTask", 2048, NULL, 1, NULL, 1);            // Rendah

        Serial.println("[SYSTEM] FreeRTOS Scheduler Dimulai...");
    }
    else
    {
        Serial.println("[ERROR] Gagal membuat objek RTOS!");
    }
}

extern "C" {
    typedef struct TraceRingBuffer TraceRingBuffer_t;
    extern TraceRingBuffer_t* RecorderDataPtr;
}

void dump_trace_serial()
{
    if (RecorderDataPtr == NULL)
    {
        Serial.println("[TRACE] Error: Tracealyzer Ring Buffer not initialized yet!");
        return;
    }
    
    // Take the Serial mutex to block any other tasks from printing!
    if (xSemaphoreTake(xSerialMutex, portMAX_DELAY) == pdTRUE)
    {
        // Stop recording first to get a stable snapshot
        vTraceStop();
        
        Serial.println("---START_TRACE_DUMP---");
        uint8_t* ptr = (uint8_t*)RecorderDataPtr;
        size_t size = sizeof(TraceRingBuffer_t);
        for (size_t i = 0; i < size; i++)
        {
            if (ptr[i] < 16) Serial.print("0");
            Serial.print(ptr[i], HEX);
        }
        Serial.println();
        Serial.println("---END_TRACE_DUMP---");
        
        // Resume recording
        vTraceEnable(TRC_START);
        
        // Release the mutex
        xSemaphoreGive(xSerialMutex);
    }
}

void loop()
{
    if (Serial.available())
    {
        char c = Serial.read();
        if (c == 'd' || c == 'D')
        {
            dump_trace_serial();
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ==========================================
// REALISASI TASK MASING-MASING ANGGOTA
// ==========================================

// --- TUGAS ANGGOTA 1: TASK MANAGEMENT & SCHEDULING ---
void vTaskSensorAcquisition(void *pvParameters)
{
    SensorData dataMentah;
    const TickType_t xDelay1000ms = pdMS_TO_TICKS(1000);

    for (;;)
    {
        float t = dht.readTemperature();

        if (isnan(t))
        {
            dataMentah.temperature = 0.0;
        }
        else
        {
            dataMentah.temperature = t;
        }

        memset(dataMentah.ciphertext, 0, sizeof(dataMentah.ciphertext));

        // Kirim ke Crypto Task via xSensorQueue
        xQueueSend(xSensorQueue, &dataMentah, 0);

        vTaskDelay(xDelay1000ms);
    }
}

// --- TUGAS ANGGOTA 2: INTER-TASK SYNCHRONIZATION & DATA ENCRYPTION ---
void vTaskCryptoProcess(void *pvParameters)
{
    SensorData dataSistem;

    for (;;)
    {
        // 1. Ambil data mentah dari xSensorQueue (Aman & Thread-safe) [cite: 71]
        if (xQueueReceive(xSensorQueue, &dataSistem, portMAX_DELAY) == pdTRUE)
        {

            // 2. Simulasi Kriptografi Ringan: Mengonversi float suhu ke string lalu di-XOR
            char teksMentah[16];
            dtostrf(dataSistem.temperature, 4, 2, teksMentah); // Misal "25.50"

            // Proses Enkripsi Obfuscation per karakter
            for (int i = 0; i < strlen(teksMentah); i++)
            {
                dataSistem.ciphertext[i] = teksMentah[i] ^ KUNCI_XOR;
            }
            dataSistem.ciphertext[strlen(teksMentah)] = '\0'; // Penutup string

            // 3. Oper data terenkripsi ke xProcessedQueue untuk dicetak Task 4
            xQueueSend(xProcessedQueue, &dataSistem, 0);
        }
    }
}

// --- TUGAS ANGGOTA 3: ISR & DEFERRED PROCESSING ---
void vTaskAlertSystem(void *pvParameters)
{
    uint32_t ulNotificationValue;

    for (;;)
    {
        // 1. Task tertidur pulas sampai dibangunkan oleh ISR tombol [cite: 71]
        ulNotificationValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (ulNotificationValue > 0)
        {
            // 2. Ambil gembok Mutex sebelum mencetak agar pesan tidak tabrakan [cite: 71]
            if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                Serial.println("\n================================================");
                Serial.println("[WARNING] PHYSICAL TAMPERING DETECTED! ALARM ACTIVE!");
                Serial.println("================================================");
                xSemaphoreGive(xSerialMutex);
            }

            // 3. Nyalakan Lampu Alarm Fisik (LED Merah Berkedip Cepat)
            for (int i = 0; i < 6; i++)
            {
                digitalWrite(PIN_LED, HIGH);
                vTaskDelay(pdMS_TO_TICKS(100));
                digitalWrite(PIN_LED, LOW);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
}

// --- TUGAS ANGGOTA 4: MEMORY, I/O MANAGEMENT & LOG ---
void vTaskSerialOutput(void *pvParameters)
{
    SensorData dataSiapCetak;
    const TickType_t xDelay1000ms = pdMS_TO_TICKS(1000);

    for (;;)
    {
        // 1. Ambil data terenkripsi dari xProcessedQueue
        if (xQueueReceive(xProcessedQueue, &dataSiapCetak, portMAX_DELAY) == pdTRUE)
        {

            // 2. Ambil kunci Mutex untuk mengamankan fungsi cetak UART [cite: 71]
            if (xSemaphoreTake(xSerialMutex, portMAX_DELAY) == pdTRUE)
            {

                Serial.print("[LOG] Plaintext Suhu: ");
                Serial.print(dataSiapCetak.temperature);
                Serial.print(" C | Ciphertext (Enkripsi): ");

                // Mencetak karakter enkripsi (mungkin berupa simbol aneh karena hasil XOR)
                for (int i = 0; i < strlen(dataSiapCetak.ciphertext); i++)
                {
                    Serial.print((unsigned char)dataSiapCetak.ciphertext[i], HEX);
                    Serial.print(" ");
                }
                Serial.println();

                // 3. BONUS MANDAT: Monitor penggunaan sisa RAM (Stack) per Task [cite: 71]
                Serial.print("[MONITOR STACK] Sisa Memory Ruang Kerja Task ini: ");
                Serial.print(uxTaskGetStackHighWaterMark(NULL));
                Serial.println(" bytes");
                Serial.println("------------------------------------------------");

                xSemaphoreGive(xSerialMutex); // Kembalikan gembok
            }
        }
        vTaskDelay(xDelay1000ms);
    }
}

extern "C" void app_main()
{
    initArduino();
    setup();
    for (;;) {
        loop();
    }
}
