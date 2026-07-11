# DropPlayer - Minimal Ses Çalar (C / GTK3 / miniaudio)

Sadece sürükle-bırak ile çalışan basit, modern görünümlü bir ses çalar yazdım.

## Özellikler

- Sürükle-bırak veya dosya seçici ile ses dosyası ekleme
- Oynat / Duraklat / Durdur / Önceki / Sonraki
- ±10s ileri/geri sarma + seek bar
- 3 oynatma modu: Tek / Döngü / Rastgele
- Karanlık / Aydınlık tema (sağ üstteki güneş/ay düğmesi)
- Dosya bittiğinde mod'a göre otomatik devam

## Format desteği

mp3, wav, ogg, flac, aac, m4a, opus, wma

## Derleme

Bağımlılıklar:
```bash`
# Debian/Ubuntu
sudo apt install gcc libgtk-3-dev libasound-dev libpulse-dev

# Arch
sudo pacman -S gtk3 alsa-lib libpulse
Derleme:
git clone <repo>
cd dropplayer
make
Kullanım
# Boş başlat, dosyaları sürükle bırak
./dropplayer

# Doğrudan dosya ile başlat
./dropplayer ~/Muzik/sarki1.mp3 ~/Muzik/sarki2.ogg
Kaynak kodu yapısı
dropplayer/
├── main.c          # Tek dosya, ~900 satır
├── miniaudio.h     # Tek header ses kütüphanesi
└── Makefile
Tek .c dosyasında her şey. miniaudio.h tek header olarak projeye dahil, ayrıca indirmeye gerek yok.
Teknik notlar
- miniaudio: Tek header, sıfır extern bağımlılık, mp3/wav/ogg/flac decode ediyor
- GTK3: Arayüz için, CSS ile tema desteği
- Audio thread safety: miniaudio callback'leri audio thread'inden çalışıyor, g_idle_add ile main thread'e dispatch ediliyor
- Track sonu tespiti: ma_sound_at_end() ile 250ms aralıkla kontrol
- Crash handler: SIGSEGV/ABRT/FPE/BUS için backtrace, loglama /tmp/dropplayer.log'a yazılıyor
- Pencere sürükleyerek dosya bırakma GTK'nın native DnD mekanizması ile
