# Çok İstemcili TCP Dosya Sunucusu

GTK 3 arayüzü ile yönetilen, çok istemcili TCP tabanlı dosya sunucusu ve
istemcisi. Sunucu sahibi paylaşmak istediği klasör/dosyaları ağaç görünümünde
seçer; istemciler bağlanarak izinli dosyaları listeler (`LIST`), indirir (`GET`)
ve sunucuya dosya yükler (`PUT`). Dizine yeni dosya eklendiğinde sunucu bunu
otomatik algılar ve bağlı istemcileri `NOTIFY` mesajıyla haberdar eder.

## Amaç

C ve POSIX API kullanarak; soket programlama, çoklu thread, senkronizasyon,
dosya G/Ç, dizin izleme ve temel ağ güvenliği konularını kapsayan uçtan uca
bir dosya paylaşım uygulaması geliştirmek.

## Tasarım

### Sunucu (`server.c`)

GTK ana thread'i kullanıcı arayüzünü yönetir. Ayrı bir *kabul thread'i*
(`server_thread`) `accept()` döngüsünü çalıştırır. Her bağlanan istemci için
ayrı bir *client thread'i* (`client_handler`) açılır (thread-per-client).

Sunucu, GUI ağacından üretilen tek bir metin tamponuna
(`cached_list_response`) sahiptir; tüm istemciler aynı tampondan okur.
Sunucu başlatılırken ağaçtaki onay durumları (`build_cached_list_recursive`)
bu tampona yazılır.

**inotify ile otomatik dosya izleme**: Sunucu çalışırken `inotify_watcher`
thread'i çalışma dizinini (`IN_CREATE | IN_DELETE | IN_MOVED_FROM |
IN_MOVED_TO | IN_CLOSE_WRITE`) izler. İlgili bir değişiklik algılandığında:
1. Ardışık hızlı olaylar 200 ms beklenerek birleştirilir.
2. `rebuild_cache_from_disk` dizini doğrudan tarayarak önbelleği yeniler;
   önceki erişim izinleri (`ticked` değerleri) korunur, yeni dosyalar
   varsayılan olarak erişilebilir (`ticked=1`) gelir.
3. `broadcast_notify` tüm WATCH abonelerine `NOTIFY\n` gönderir.
4. `g_idle_add(refresh_server_tree_idle)` sunucu GUI ağacını günceller.

**Push bildirim aboneleri**: `WATCH` komutunu gönderen istemci soketleri
`notify_sockets[]` dizisinde tutulur (maks. 64 abone). Dizin değişiminde
hepsine `NOTIFY\n` yayınlanır.

**Aktif istemci varken durdurma**: Sunucu durdurulurken bağlı istemci varsa
onay diyaloğu açılır; kullanıcı vazgeçebilir.

**Dahili dosya filtresi**: `is_relevant_filename` fonksiyonu; gizli dosyalar
(`'.'` ile başlayanlar), `server.log`, `server` ve `client` ikili dosyalarını
listeden dışarıda bırakır.

### İstemci (`client.c`)

GTK arayüzünden sunucu IP/port'una bağlanır; `LIST` yanıtını alıp ağaç
olarak gösterir, kullanıcı seçimine göre `GET`/`PUT` yapar.

Bağlantı kurulurken **ayrı bir bildirim soketi** açılır. Bu soket `WATCH\n`
komutu gönderir ve `notify_listener` thread'i ile dinlenir. Sunucudan
`NOTIFY\n` geldiğinde `g_idle_add(refresh_list_idle)` çağrısıyla dosya
listesi otomatik olarak yenilenir.

Dosya transferleri **ayrı bir worker thread'de** yürütülür; GUI ana thread'i
bloke olmaz. İlerleme, aktarılan byte miktarı ve anlık hız bir
`GtkProgressBar` ile canlı gösterilir. Aynı anda yalnızca bir transfer
aktif olabilir (`transfer_active` bayrağı).

**Zaman aşımlı bağlanma**: `connect_with_timeout` (5 sn) non-blocking soket
+ `select()` kullanır. Erişilemeyen bir IP'de arayüz kilitlenmez.

### İletişim Protokolü (satır temelli)

| Komut | İstek | Yanıt |
|---|---|---|
| `LIST` | `LIST\n` | `depth\|ticked\|is_dir\|name\|path\n` satırları + `__END_OF_LIST__\n` |
| `GET` | `GET <path>\n` | `OK <size>\n` ardından `size` byte ham veri; hata: `ERROR <msg>\n` |
| `PUT` | `PUT <name> <size>\n` | `OK\n` ardından istemci `size` byte ham veri yollar; hata: `ERROR <msg>\n` |
| `WATCH` | `WATCH\n` | Sunucu bağlantıyı push bildirimi için açık tutar; değişimde `NOTIFY\n` yollar |

### Yetkilendirme

- `GET`: Yalnızca önbellekte `ticked=1` olan yollar indirilebilir.
- `PUT`: Yalnızca kök çalışma dizinine, alt yol (`/` veya `\\`) içermeyen
  dosya adıyla yükleme yapılabilir.
- `../` veya `..\` içeren tüm istekler anında reddedilir.

## Kullanılan Sistem Programlama Kavramları

- **TCP soketleri**: `socket`, `bind`, `listen`, `accept`, `connect`,
  `send`/`recv`. Sunucu `DEFAULT_PORT = 8765` üzerinde dinler; gerçek port
  `getsockname` ile alınıp arayüzde gösterilir.
- **Çoklu thread**: POSIX `pthread`. Sunucuda her istemciye bir thread
  (`pthread_create` + `pthread_detach`). İstemcide her dosya transferi için
  ayrı bir worker thread; böylece GET/PUT sırasında arayüz donmaz. Ayrıca
  sunucuda `inotify_watcher`, istemcide `notify_listener` arka plan
  thread'leri çalışır.
- **Thread ↔ GUI senkronizasyonu**: GTK thread-safe olmadığından, worker
  thread'ler arayüze **doğrudan dokunmaz**; ilerleme/sonuç/log/bildirim
  güncellemeleri `g_idle_add` ile GTK ana döngüsüne aktarılır. İstemcide
  `post_progress`/`post_result`, sunucuda `append_log_to_view`/
  `refresh_clients_label`/`refresh_server_tree_idle` bu köprüyü kurar.
- **Senkronizasyon primitifleri**:
  - `pthread_mutex_t log_mutex` — `server.log` dosyasına yazımı serileştirir.
  - `pthread_rwlock_t cache_rwlock` — paylaşımlı `cached_list_response`:
    çok sayıda istemci aynı anda *okur* (rdlock), GUI/inotify thread'i nadir
    aralıklarla *yazar* (wrlock). Okuma-ağırlıklı yük için rwlock seçildi.
  - `pthread_mutex_t clients_mutex` — `connected_clients` sayacını korur.
  - `pthread_mutex_t notify_mutex` — `notify_sockets[]` dizisini korur.
- **inotify**: `inotify_init` + `inotify_add_watch` ile çalışma dizini
  izlenir. Olaylar `read` ile okunur; ardışık olaylar birleştirilerek gereksiz
  yeniden tarama engellenir.
- **Dosya G/Ç**: `fopen`/`fread`/`fwrite`, 4096 byte'lık bloklarla parçalı
  transfer.
- **Dizin tarama**: `opendir`/`readdir`/`stat` + `S_ISDIR` ile özyinelemeli
  ağaç oluşturma. Maksimum derinlik `MAX_TREE_DEPTH = 16`.
- **Zaman ölçümü**: `clock_gettime(CLOCK_MONOTONIC, ...)` ile transfer süresi
  ve MB/s hızı hesaplanır, log'a yazılır.
- **Güvenlik**:
  - Path traversal: `../` ve `..\` içeren istekler reddedilir.
  - Whitelist: GET yalnızca önbellekte `ticked=1` olan yollara izin verir.
  - PUT: dosya adında `/` veya `\` varsa reddedilir; yalnızca kök dizine yazılır.
  - Dahili dosya filtresi: `server.log`, `server`, `client` ve gizli dosyalar
    (`'.'` ile başlayanlar) istemciye listelenmez.
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
eşzamanlı bağlantı sayısını anlık olarak yansıtır. Sunucu çalışırken dizine
eklenen yeni dosyalar otomatik algılanır ve bağlı istemciler bildirilir.

İstemci (aynı veya farklı makinede):

```
./client
```

IP ve port'u girip **Bağlan**'a basın. Ağaçtan dosya seçip **⬇ Seçili
Dosyayı İndir** (GET) veya yerel bir dosya için **⬆ Sunucuya Dosya Yükle**
(PUT) yapın. Transfer sırasında alttaki **ilerleme çubuğu** yüzde, anlık hız
ve tahmini kalan süreyi gösterir; arayüz boyunca donma yaşanmaz. Sunucu
tarafında dizin değişimi olduğunda istemci listesi otomatik yenilenir.

## Testler

Aşağıdaki senaryolar manuel olarak test edildi:

1. **Tek istemci LIST/GET/PUT**: Küçük (kB) ve büyük (yüzlerce MB) dosyalarda
   bütünlük (`sha256sum` ile karşılaştırma).
2. **Eşzamanlı çoklu istemci**: 4 istemci aynı anda farklı dosyalar indirirken
   sunucu kararlı kaldı; her bağlantı için ayrı `client_handler` thread'i
   log'da görüldü.
3. **Yetkilendirme**: GUI'de işareti kaldırılmış bir dosya için `GET` denemesi
   `ERROR Erisim Engellendi` ile reddedildi; istemci GUI'de `(Erişim Engellendi)`
   etiketiyle gösterdi.
4. **Path traversal**: `GET ../etc/passwd` denemesi `ERROR Yetkisiz erisim`
   ile reddedildi ve log'a düşürüldü.
5. **PUT alt yol denemesi**: `PUT ../x 10` reddedildi.
6. **inotify – otomatik yenileme**: Sunucu çalışırken dizine yeni bir dosya
   kopyalandığında bağlı istemcilerin listesi otomatik olarak güncellendi;
   el ile yenileme gerekmedi.
7. **Sunucu durdurma – aktif istemci**: Aktif bağlantı varken sunucu durdurma
   butonu onay diyaloğu açtı; kullanıcı "Hayır" dediğinde sunucu çalışmaya
   devam etti.
8. **Geçersiz komut**: Bilinmeyen komut için `HATA: Gecersiz komut.`
   döndürüldü; bağlantı korundu.
9. **Bağlantı zaman aşımı**: Kapalı bir adrese bağlanma denemesi 5 saniye
   sonra hata mesajıyla sonuçlandı; GUI kilitlenmedi.

## Performans Değerlendirmesi

Her `GET` ve `PUT` işlemi sonunda log'a şu format yazılır:

```
GET <yol> (boyut: N byte, sure: T sn, hiz: X MB/s)
PUT <ad>  (boyut: N byte, sure: T sn, hiz: X MB/s)
```

Bu sayede:

- Dosya boyutuna göre throughput karşılaştırılabilir.
- Aynı dosyayı farklı sayıda eşzamanlı istemciyle indirip toplam ve
  istemci-başı hız değişimi gözlemlenebilir (thread-per-client modelinin
  ölçeklenmesi).
- I/O ve ağ gecikmesinin (`localhost` vs LAN vs WAN) etkisi ölçülebilir.

Loopback üzerinde tipik tek-istemci throughput, 4 KB blok ve sıradan SATA SSD
ile ~200–400 MB/s mertebesindedir; ağ üzerinde bağlantı bant genişliği
sınırlayıcı olur.

## Karşılaşılan Problemler ve Çözümler

- **Race condition – paylaşımlı liste tamponu**: İlk sürümde
  `cached_list_response` herhangi bir kilit olmadan birden çok client thread'i
  tarafından okunuyor, GUI thread'i tarafından da `free` edilebiliyordu;
  potansiyel use-after-free. Çözüm: `pthread_rwlock_t` ile okuma/yazma
  ayrımı.
- **inotify önbellekle çakışması**: inotify thread'i `rebuild_cache_from_disk`
  çağırırken client thread'leri aynı tamponu okuyordu. Çözüm: yeniden yapım
  öncesinde write-lock alınır; okuyucular yeniden yapım bitene kadar bekler.
  Önceki `ticked` değerleri `save_ticks_from_cache` ile saklanıp yeni
  önbelleğe aktarılır.
- **İstemci tarafında stack overflow riski**: LIST yanıtı 1 MB'lık sabit
  stack tamponuna `strcat` ile birikiyordu. Çözüm: gerekirse büyüyen heap
  tamponu (`malloc`/`realloc`/`memcpy`).
- **GET'te zayıf yetkilendirme**: Önceki sürüm yalnızca `strstr` ile yolun
  cache içinde geçip geçmediğine bakıyordu; işaretsiz bir dosya yine de
  indirilebiliyordu. Çözüm: ilgili satırı bulup `ticked` alanını da kontrol
  eden kesin doğrulama.
- **Sınırsız dizin derinliği**: `populate_tree` özyinelemesi büyük ağaçlarda
  stack'i tüketebilirdi. Çözüm: `MAX_TREE_DEPTH = 16` ile sınır.
- **Protokol senkronizasyonu (PUT)**: `fopen` başarısızsa istemci yine veri
  yollayabilirdi. Çözüm: sunucu `OK\n` ancak `fopen` başarılıysa gönderir;
  istemci `OK` gelmedikçe veri yollamaz.
- **GUI donması (büyük dosya transferi)**: GET/PUT soket G/Ç'si GTK ana
  thread'inde çalışıyor, transfer boyunca arayüz yanıt vermiyordu. Çözüm:
  transferi ayrı worker thread'e taşıma; ilerleme/sonuç ana thread'e
  `g_idle_add` ile aktarılıyor.
- **Bloklayan `connect()`**: Erişilemeyen bir IP'ye bağlanırken arayüz
  varsayılan TCP zaman aşımı kadar (dakikalar) kilitleniyordu. Çözüm:
  non-blocking soket + `select()` ile 5 sn'lik `connect_with_timeout`.
- **Sabit boyutlu `iters` dizisi (istemci)**: LIST ayrıştırmasında
  `GtkTreeIter iters[100]` kullanılıyordu; bozuk/aşırı derin bir satır
  taşmaya yol açabilirdi. Çözüm: diziyi `MAX_TREE_DEPTH + 1` ile
  boyutlandırma ve `depth` için sınır kontrolü.
- **Dahili dosya kirliliği**: `server.log`, `server` ve `client` ikilileri
  listelenerek istemcinin bunlara erişmeye çalışması anlamsız hatalara yol
  açıyordu. Çözüm: `is_relevant_filename` filtresi bu dosyaları dışarıda
  bırakır.
