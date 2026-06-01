# Çok İstemcili TCP Dosya Sunucusu

GTK 3 arayüzü ile yönetilen, çok istemcili TCP tabanlı dosya sunucusu ve
istemcisi. Sunucu sahibi paylaşmak istediği klasör/dosyaları ağaç görünümünde
seçer, istemciler bağlanarak izinli dosyaları listeler (`LIST`), indirir (`GET`)
ve sunucuya dosya yükler (`PUT`).

## Amaç

C ve POSIX API kullanarak; soket programlama, çoklu thread, senkronizasyon,
dosya G/Ç ve temel ağ güvenliği konularını kapsayan uçtan uca bir dosya
paylaşım uygulaması geliştirmek.

## Tasarım

- **Sunucu (`server.c`)**: GTK ana thread'i kullanıcı arayüzünü yönetir. Ayrı bir
  *kabul thread'i* (`server_thread`) `accept()` döngüsünü çalıştırır. Her bağlanan
  istemci için ayrı bir *client thread'i* (`client_handler`) açılır
  (thread-per-client). Sunucu, GUI'deki ağaçtan üretilen tek bir metin tampona
  (`cached_list_response`) sahiptir; tüm istemciler aynı tampondan okur.
- **İstemci (`client.c`)**: GTK arayüzünden sunucu IP/port'una bağlanır, `LIST`
  yanıtını alıp ağaç olarak gösterir, kullanıcı seçimine göre `GET`/`PUT` yapar.
  Dosya transferleri **ayrı bir worker thread'de** yürütülür; GUI ana thread'i
  bloke olmaz. İlerleme bir `GtkProgressBar` ile canlı gösterilir.
- **Protokol** (satır temelli):
  - `LIST\n` → sunucu, satır başına `depth|ticked|is_dir|name|path\n`
    formatında listeyi gönderir, `__END_OF_LIST__\n` ile sonlandırır.
  - `GET <path>\n` → sunucu `OK <size>\n` döner, ardından `size` byte ham veri.
    Reddederse `ERROR <msg>\n`.
  - `PUT <name> <size>\n` → sunucu `OK\n` döner, istemci `size` byte ham veri
    yollar. Reddederse `ERROR <msg>\n`.
- **Yetkilendirme**: İstemci sadece sunucu GUI'sinde işaretli (`ticked=1`)
  yollara `GET` yapabilir. `PUT` sadece kök çalışma dizinine, alt yol
  içermeyen dosya adıyla yapılabilir.

## Kullanılan Sistem Programlama Kavramları

- **TCP soketleri**: `socket`, `bind`, `listen`, `accept`, `connect`,
  `send`/`recv`. Sunucu portu `0` ile bind edilip kernel'in atadığı port
  arayüzde gösterilir.
- **Çoklu thread**: POSIX `pthread`. Sunucuda her istemciye bir thread
  (`pthread_create` + `pthread_detach`). İstemcide her dosya transferi için ayrı
  bir worker thread; böylece GET/PUT sırasında arayüz donmaz.
- **Thread ↔ GUI senkronizasyonu**: GTK thread-safe olmadığından, worker
  thread'ler arayüze **doğrudan dokunmaz**; ilerleme/sonuç/log güncellemeleri
  `g_idle_add` ile GTK ana döngüsüne aktarılır. İstemcide
  `post_progress`/`post_result`, sunucuda `append_log_to_view`/
  `refresh_clients_label` bu köprüyü kurar.
- **Zaman aşımlı bağlanma**: İstemci `connect()`'i non-blocking soket +
  `select()` ile sarmalar (`connect_with_timeout`, 5 sn). Erişilemeyen bir IP'ye
  bağlanırken arayüz dakikalarca kilitlenmez.
- **Senkronizasyon**:
  - `pthread_mutex_t log_mutex` — log dosyasına yazımı serileştirir.
  - `pthread_rwlock_t cache_rwlock` — paylaşımlı `cached_list_response` tamponu:
    çok sayıda istemci aynı anda *okur* (rdlock), GUI thread'i nadir aralıklarla
    *yazar* (wrlock). Okuma-ağırlıklı yük için rwlock seçildi.
- **Dosya G/Ç**: `fopen`/`fread`/`fwrite`, 4096 byte'lık bloklarla parçalı
  transfer.
- **Dizin tarama**: `opendir`/`readdir`/`stat` + `S_ISDIR` ile özyinelemeli
  ağaç oluşturma. Maksimum derinlik `MAX_TREE_DEPTH = 16`.
- **Zaman ölçümü**: `clock_gettime(CLOCK_MONOTONIC, ...)` ile transfer süresi
  ve MB/s hızı hesaplanır, log'a yazılır.
- **Güvenlik**:
  - Path traversal: `../` ve `..\\` içeren istekler reddedilir.
  - Whitelist: GET yalnızca GUI'de işaretli yollara izin verir.
  - PUT: dosya adında `/` veya `\\` varsa reddedilir.
- **Hata yönetimi**: `perror` + zaman damgalı `server.log` (thread-safe).
  Hatalı istek sunucuyu düşürmez; bağlantı kapanışı ve istemci hatası
  bireysel thread içinde toplanır.

## Çalıştırma Adımları

Bağımlılıklar (Debian/Ubuntu):

```
sudo apt install build-essential libgtk-3-dev pkg-config
```

Derleme:

```
make
```

Sunucu:

```
./server
```

GUI açıldığında paylaşmak istediğiniz dosya/klasörlerin yanındaki onay
kutusunu işaretleyin, **Sunucuyu Başlat**'a basın. Arayüzde gösterilen IP ve
port'u istemciye iletin. Pencerenin altındaki **Olay Günlüğü** paneli tüm
bağlantı/komut olaylarını canlı gösterir; üstte **Bağlı istemci** sayacı
eşzamanlı bağlantı sayısını anlık olarak yansıtır.

İstemci (aynı veya farklı makinede):

```
./client
```

IP ve port'u girip **Bağlan**'a basın. Ağaçtan dosya seçip **GET**, ya da
yerel bir dosya için **PUT** yapın. Transfer sırasında alttaki **ilerleme
çubuğu** yüzde ve aktarılan byte miktarını gösterir; arayüz boyunca donma
yaşanmaz (transfer ayrı thread'de yürür).

## Testler

Aşağıdaki senaryolar manuel olarak test edildi:

1. **Tek istemci LIST/GET/PUT**: Küçük (kB) ve büyük (yüzlerce MB) dosyalarda
   bütünlük (`sha256sum` ile karşılaştırma).
2. **Eşzamanlı çoklu istemci**: 4 istemci aynı anda farklı dosyalar indirirken
   sunucu kararlı kaldı; her bağlantı için ayrı `client_handler` thread'i log'da
   görüldü.
3. **Yetkilendirme**: GUI'de işareti kaldırılmış bir dosya için `GET` denemesi
   `ERROR Erisim Engellendi` ile reddedildi.
4. **Path traversal**: `GET ../etc/passwd` denemesi `ERROR Yetkisiz erisim`
   ile reddedildi ve log'a düşürüldü.
5. **PUT alt yol denemesi**: `PUT ../x 10` reddedildi.
6. **Sunucu durdurma**: Aktif bağlantı varken sunucu durdurulduğunda
   istemciler temiz şekilde bağlantı koptu mesajı aldı, sunucu çökmedi.
7. **Geçersiz komut**: Bilinmeyen komut için `HATA: Gecersiz komut.`
   döndürüldü; bağlantı korundu.

## Performans Değerlendirmesi

Her `GET` ve `PUT` işlemi sonunda log'a şu format yazılır:

```
GET <yol> (boyut: N byte, sure: T sn, hiz: X MB/s)
PUT <ad>  (boyut: N byte, sure: T sn, hiz: X MB/s)
```

Bu sayede:

- Dosya boyutuna göre throughput karşılaştırılabilir.
- Aynı dosyayı farklı sayıda eşzamanlı istemciyle indirip toplam ve istemci-başı
  hız değişimi gözlemlenebilir (thread-per-client modelinin ölçeklenmesi).
- I/O ve ağ gecikmesinin (`localhost` vs LAN vs WAN) etkisi ölçülebilir.

Loopback üzerinde tipik tek-istemci throughput, 4 KB blok ve sıradan SATA SSD
ile ~200–400 MB/s mertebesindedir; ağ üzerinde bağlantı bant genişliği
sınırlayıcı olur.

## Karşılaşılan Problemler

- **Race condition – paylaşımlı liste tamponu**: İlk sürümde
  `cached_list_response` herhangi bir kilit olmadan birden çok client thread'i
  tarafından okunuyor, GUI thread'i tarafından da `free` edilebiliyordu;
  potansiyel use-after-free. Çözüm: `pthread_rwlock_t` ile okuma/yazma
  ayrımı.
- **İstemci tarafında stack overflow riski**: LIST yanıtı 1 MB'lık sabit
  stack tamponuna `strcat` ile birikiyordu. Çözüm: gerekirse büyüyen heap
  tamponu (`malloc`/`realloc`/`memcpy`).
- **GET'te zayıf yetkilendirme**: Önceki sürüm yalnızca `strstr` ile yolun
  cache içinde geçip geçmediğine bakıyordu; bir kullanıcının işaretsiz
  bıraktığı dosya yine de indirilebiliyordu. Çözüm: ilgili satırı bulup
  `ticked` alanını da kontrol eden bir doğrulama.
- **Sınırsız dizin derinliği**: `populate_tree` özyinelemesi büyük ağaçlarda
  stack'i tüketebilirdi. Çözüm: `MAX_TREE_DEPTH` ile sınır.
- **Protokol senkronizasyonu (PUT)**: `fopen` başarısızsa istemci yine
  veri yollayabilirdi; istemci `OK` aksi halde göndermiyor olarak güncellendi.
- **GUI donması (büyük dosya transferi)**: GET/PUT soket G/Ç'si GTK ana
  thread'inde çalışıyor, transfer boyunca arayüz yanıt vermiyordu. Çözüm:
  transferi ayrı worker thread'e taşıma; ilerleme/sonuç ana thread'e
  `g_idle_add` ile aktarılıyor (GTK thread-safe olmadığından zorunlu).
- **Bloklayan `connect()`**: Erişilemeyen bir IP'ye bağlanırken arayüz
  varsayılan TCP zaman aşımı kadar (dakikalar) kilitleniyordu. Çözüm:
  non-blocking soket + `select()` ile 5 sn'lik `connect_with_timeout`.
- **Sabit boyutlu `iters` dizisi (istemci)**: LIST ayrıştırmasında
  `GtkTreeIter iters[100]` kullanılıyordu; bozuk/aşırı derin bir satır taşmaya
  yol açabilirdi. Çözüm: diziyi `MAX_TREE_DEPTH` ile boyutlandırma ve `depth`
  için sınır kontrolü.
