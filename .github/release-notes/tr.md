**Yenilikler**

- Savaşta kare hızını sınırlayan thread, yani GPU komut işlemcisi, artık grafik sürücüsünü beklemiyor: ürettiği Vulkan komutları artık kendi thread'inde kaydedilip gönderiliyor. Sürücü bu thread'in zamanının yaklaşık beşte birini alıyordu. Bu, *Record GPU Commands on a Separate Thread* ayarı, varsayılan olarak açık. Bir oyunda sorun çıkarırsa oradan kapatın ya da `config.json` dosyasının `GPU` bölümünde `threaded_command_recording` değerini `false` yapın, ve lütfen log'u gönderin.
- Log artık bu thread'in süresini ("command recording" ve "Sampler: recorder"), ve her on saniyede bir savaşta GPU'nun en çok vaktini alan compute shader'ları ("Dispatches timed one by one") gösteriyor; sırada neyi hızlandıracağımızı bulmak için.
