# Secure Data Logger with FreeRTOS & LUB Schedulability Test

Proyek ini adalah sistem logging data sensor suhu yang aman menggunakan **FreeRTOS** pada mikrokontroler **ESP32**. Proyek ini dilengkapi dengan modul pengukuran waktu eksekusi riil (*Worst-Case Execution Time* / WCET) dan analisis kelayakan penjadwalan dinamis menggunakan uji **Least Upper Bound (LUB)** dari teorema Liu & Layland.

---

## 🚀 Fitur Utama
1. **Multi-tasking Preemptive (FreeRTOS)**: Penjadwalan task berbasis prioritas preemptive.
2. **Sensor Acquisition (Task 1)**: Mengambil data dari sensor suhu DHT22 secara periodik setiap **2000 ms** (memenuhi batas minimum sampling rate pada datasheet DHT22).
3. **Data Encryption (Task 2)**: Mengenkripsi suhu menggunakan algoritma XOR obfuscation ringan secara thread-safe via FreeRTOS Queue.
4. **Physical Tampering Alert (Task 3)**: Alarm interupsi fisik (tombol) menggunakan ISR (*Interrupt Service Routine*) dengan mekanisme *deferred processing* untuk berkedip cepat tanpa memblokir task lain. Diimplementasikan secara *event-driven* murni dengan prioritas tertinggi (prioritas 4) untuk menjamin latensi respons $< 5\text{ ms}$.
5. **Serial Monitor & LUB Analyzer (Task 4)**: 
   * Menampilkan data plaintext dan enkripsi (hex).
   * Memonitor penggunaan stack memori per task (`uxTaskGetStackHighWaterMark`).
   * Melakukan uji kelayakan penjadwalan **Least Upper Bound (LUB)** secara dinamis setiap periode menggunakan perhitungan utilisasi CPU riil.

---

## 📋 Model Parameter Penjadwalan Task

Berikut adalah tabel pemodelan task pada sistem untuk analisis penjadwalan LUB:

| Nama Task | Handler / Fungsi | Prioritas | Tipe Penjadwalan | Periode / Min Inter-arrival ($T_i$) | Keterangan |
| :--- | :--- | :---: | :--- | :---: | :--- |
| **`AlertTask`** | `vTaskAlertSystem` | 4 | **Event-Driven** | 5000 ms | Hard Real-Time, dipicu interupsi tombol fisik |
| **`SensorTask`** | `vTaskSensorAcquisition` | 3 | Periodik | 2000 ms | Melakukan pembacaan berkala DHT22 |
| **`CryptoTask`** | `vTaskCryptoProcess` | 2 | Periodik (Pipeline) | 2000 ms | Melakukan enkripsi XOR data dari queue |
| **`SerialTask`** | `vTaskSerialOutput` | 1 | Periodik (Pipeline) | 2000 ms | Mencetak log serial & melakukan analisis LUB |

---

## 🔌 Skema Sirkuit & Koneksi PIN
Konfigurasi hardware pada ESP32 diatur sebagai berikut:

* **Sensor DHT22**: Pin data terhubung ke **GPIO 15**
* **Pushbutton (Alarm Tombol)**: Terhubung ke **GPIO 12** (menggunakan internal pull-up, aktif LOW saat ditekan)
* **LED Alarm Merah**: Terhubung ke **GPIO 2** (aktif HIGH)

---

## 💻 Cara Menjalankan

### 1. Simulasi dengan Wokwi (Tanpa Hardware Fisik)
Proyek ini dilengkapi dengan konfigurasi Wokwi (`diagram.json` & `wokwi.toml`). Anda bisa menjalankannya langsung di VS Code:
1. Pastikan Anda telah menginstal ekstensi **Wokwi Simulator** di VS Code.
2. Buka file [diagram.json](file:///c:/Users/Lenovo-ex/Documents/RTOSProject/diagram.json).
3. Klik tombol **Start Simulation** di pojok kanan atas diagram (atau tekan `F5` / jalankan lewat Command Palette `Ctrl+Shift+P` -> `Wokwi: Start Simulator`).
4. Log data sensor, status stack RAM, dan **Hasil Uji Kelayakan Penjadwalan LUB** akan tercetak langsung pada serial terminal simulator di VS Code.
5. Anda bisa menekan tombol hijau virtual di simulator untuk memicu interupsi alarm fisik.

### 2. Menggunakan ESP32 Fisik
Jika Anda ingin memasangnya ke modul ESP32 fisik:
1. Hubungkan ESP32 ke port USB komputer Anda.
2. Jalankan perintah kompilasi dan unggah program menggunakan PlatformIO CLI di VS Code Terminal:
   ```bash
   pio run --target upload --target monitor
   ```
3. Terminal akan langsung berubah menjadi Serial Monitor pada baudrate 115200 setelah proses unggah selesai.

---

## 📊 Hasil Analisis Kelayakan Penjadwalan (LUB)
Pada Serial Monitor, sistem akan mencetak tabel kelayakan penjadwalan secara dinamis seperti di bawah ini:
```text
================================================
[UJI LEAST UPPER BOUND (LUB) - SCHEDULABILITY]
Task 1 (Sensor) WCET : C1 = 26.5000 ms
Task 2 (Crypto) WCET : C2 = 0.0500 ms
Task 3 (Serial) WCET : C3 = 5.2000 ms
Task 4 (Alert) WCET  : C4 = 0.0120 ms
Total Utilitas CPU (U)  : 0.015894 (1.5894%)
Batas LUB (n=4)         : 0.756828 (75.6828%)
Status Penjadwalan      : [PASS] Dijamin Schedulable (U <= LUB)
================================================
```
* **PASS**: Total utilitas CPU ($U$) di bawah batas LUB untuk 4 tugas ($75.68\%$). Sistem dijamin 100% aman dan tidak akan melewatkan deadline.
* **EXCEEDED**: Total utilitas berada di antara batas LUB dan $100\%$. Tugas mungkin masih dapat berjalan dengan sukses tetapi tidak dijamin secara matematis oleh LUB (memerlukan Response Time Analysis / RTA).
* **FAIL**: Total utilitas melebihi $100\%$, menunjukkan sistem kelebihan beban dan tidak layak dijadwalkan.

*Catatan: Task 4 (Alert) merupakan tugas sporadic berbasis event. Dalam pemodelan LUB, tugas ini dianalisis dengan parameter waktu antar-kedatangan minimum ($T_{\text{alert}} = 5000\text{ ms}$) dan dihitung waktu eksekusi CPU aktif riilnya ($C_4$), tanpa mengikutsertakan durasi pasif berkedip LED.*

---

## 🧪 Cara Menjalankan Unit Testing
Folder `test/` disiapkan untuk PlatformIO Test Runner. Untuk menulis dan menjalankan unit testing:
1. Tulis kode pengujian Anda di dalam folder `test/` (misalnya membuat file `test_crypto.cpp`).
2. Jalankan pengujian di terminal dengan perintah:
   ```bash
   pio test
   ```
   *Catatan: PlatformIO akan otomatis mengunggah kode tes ini ke modul ESP32 Anda untuk dieksekusi.*
