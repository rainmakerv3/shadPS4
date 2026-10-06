**Yenilikler**

- Yoğun sahnelerde kare hızını sınırlayan GPU thread'i her çizim için yine daha az iş yapıyor:
  - Bir çizimin shader'ın hangi varyantını istediğini bulmak, shader'ın bellekteki yerini de karşılaştırıyordu; oyunda bazı shader'ların yüzlerce adreste kopyası var. Aramaların %13'ü ıskalayıp varyantı yeniden hesaplıyordu; artık %0.3'ü, arama başına neredeyse üç yerine yaklaşık bir karşılaştırmayla.
  - Bir doku bağlanırken artık tüm tanımı (yaklaşık 400 bayt) kopyalanmıyor, arama anahtarı daha hızlı hash'leniyor.
  - Shader'ın hash'i her çizimde oyunun belleğinden okunmuyor; daha önce bağlandığı yerde bağlanan tamponlar yerleşik bellek aramıyor, hâlâ yazılmış olarak işaretli tamponlar da oyun thread'lerinin sayfa hatasında tuttuğu kilidi atlıyor.
  - Çok kaynaklı shader'ların descriptor set'leri kayıt thread'inde yazılıyor; register yazımları ve profiler sayaçları daha ucuz.
- RTX 5080 ve Ryzen 7 9800X3D'de, Seattle'daki parkta dururken, emülatör yüksek öncelikte, sırayla üçer kez ölçüldü: 1.0.11'e göre arka planda çok şey çalışırken ortalama 46.9 -> 49.3 FPS, daha az şey çalışırken 58.6 -> 62.3 FPS. Kare hızını hâlâ GPU thread'i sınırlıyor.
