# Panduan Menjalankan di ESP32-S3 (Hardware Fisik)

## Apa yang Dibutuhkan

### Hardware
- 1x **ESP32-S3 DevKitC** (atau board ESP32-S3 lainnya)
- 1x **Sensor DHT22** (sensor suhu & kelembaban)
- 1x **Push Button** (tombol tekan)
- 1x **LED** (warna merah/kuning)
- 1x **Resistor 220Ω** (untuk LED)
- Kabel jumper secukupnya
- Breadboard
- Kabel USB Type-C (untuk koneksi ke PC)

### Software
- **VS Code** dengan ekstensi **PlatformIO IDE**
- Driver USB:
  - Kalau board kamu pakai chip **CH340** → install driver CH340
  - Kalau pakai **CP2102** → install driver CP2102
  - Kalau pakai **USB CDC native** → biasanya otomatis di Windows 10/11

---

## Langkah 1: Wiring (Rangkaian)

Sambungkan komponen ke ESP32-S3 sesuai skema berikut:

```
ESP32-S3 DevKitC
┌──────────────────┐
│                  │
│  GPIO 15 ────────┼──── DHT22 (pin DATA)
│                  │
│  3.3V ───────────┼──── DHT22 (pin VCC)
│                  │
│  GND ────────────┼──── DHT22 (pin GND)
│                  │
│  GPIO 12 ────────┼──── Push Button (sisi 1)
│                  │
│  GND ────────────┼──── Push Button (sisi 2)
│                  │
│  GPIO 2 ─────────┼──── LED (+) Anoda
│                  │      │
│                  │    [220Ω]
│                  │      │
│  GND ────────────┼──── LED (-) Katoda
│                  │
└──────────────────┘
```

### Catatan Wiring
- **DHT22**: Pin DATA ke GPIO 15, VCC ke 3.3V, GND ke GND
- **Button**: Satu kaki ke GPIO 12, kaki lain ke GND. Tidak perlu resistor pull-up eksternal karena kode sudah mengaktifkan `INPUT_PULLUP` internal
- **LED**: Anoda (+, kaki panjang) ke GPIO 2 via resistor 220Ω, Katoda (-, kaki pendek) ke GND

---

## Langkah 2: Colokkan ESP32-S3 ke PC

1. Hubungkan ESP32-S3 ke PC dengan kabel USB Type-C
2. Buka **Device Manager** (Win+X → Device Manager)
3. Cari di bagian **Ports (COM & LPT)** — catat nomor COM-nya (misal: `COM5`)
4. Kalau tidak muncul, install driver USB yang sesuai (lihat bagian Software)

---

## Langkah 3: Buka Proyek di VS Code

1. Buka VS Code
2. Buka folder proyek: **File → Open Folder → pilih folder `RTOSProject`**
3. Tunggu PlatformIO selesai menginisialisasi (icon PlatformIO di sidebar kiri)

---

## Langkah 4: Compile (Build)

Buka terminal di VS Code (`Ctrl+`` `) dan jalankan:

```bash
pio run -e esp32s3
```

Tunggu sampai muncul:
```
========================= [SUCCESS] Took XX.XX seconds =========================
```

Kalau ada error, pastikan:
- Koneksi internet aktif (pertama kali perlu download toolchain)
- PlatformIO extension terinstall dengan benar

---

## Langkah 5: Upload ke ESP32-S3

```bash
pio run -e esp32s3 -t upload
```

Kalau upload gagal, coba:
1. Tekan dan tahan tombol **BOOT** di board ESP32-S3
2. Tekan tombol **RESET** sekali (sambil tetap tahan BOOT)
3. Lepas tombol BOOT
4. Jalankan ulang perintah upload

---

## Langkah 6: Buka Serial Monitor

```bash
pio run -e esp32s3 -t monitor
```

Atau langsung compile + upload + monitor dalam satu perintah:

```bash
pio run -e esp32s3 -t upload -t monitor
```

Serial Monitor akan menampilkan:
1. **Banner startup** dengan info heap dan watchdog
2. Setiap 2 detik: **Data Log** (suhu, ciphertext, CRC, Edge AI status)
3. Setiap 2 detik: **IoT JSON Payload**
4. Setiap 2 detik: **Memory Monitor** (heap + stack)
5. Setiap 2 detik: **Schedulability Analysis** (LUB + RTA)
6. Setiap 10 detik (5 iterasi): **Runtime Trace Log**

---

## Langkah 7: Tes Tombol (ISR + Deferred Processing)

1. Tekan **push button** yang terhubung ke GPIO 12
2. Di Serial Monitor akan muncul peringatan tampering
3. LED akan berkedip 6 kali
4. Event ISR akan terekam di Runtime Trace Log berikutnya

---

## Langkah 8: Keluar dari Serial Monitor

Tekan **Ctrl+C** di terminal untuk keluar dari Serial Monitor.

---

## Troubleshooting

### Board tidak terdeteksi
- Coba kabel USB lain (pastikan kabel data, bukan charging only)
- Install driver USB yang sesuai
- Coba port USB lain di PC

### Upload gagal
- Gunakan metode BOOT+RESET (lihat Langkah 5)
- Cek apakah COM port benar: `pio device list`

### Serial Monitor kosong / karakter aneh
- Pastikan `monitor_speed = 115200` di `platformio.ini`
- Tekan tombol RESET di board setelah buka Serial Monitor

### DHT22 baca 0.00 C
- Cek wiring — pastikan pin DATA terhubung ke GPIO 15
- Pastikan VCC terhubung ke 3.3V (bukan 5V)
- Coba tambah resistor pull-up 10kΩ antara DATA dan VCC
