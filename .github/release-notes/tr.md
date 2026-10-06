**Yenilikler**

- Yoğun sahnelerde kare hızını sınırlayan GPU thread'i her çizim için yine daha az iş yapıyor:
  - Bir çizimin ya da dispatch'in yazdığı tamponların (karede 1400-1600) zaten yazılmış olarak işaretli olup olmadığı artık bir ağaçta aranmıyor; en son işaretlenenler hatırlanıyor.
  - Bir tampon bağlanırken son bariyerden beri yazılan bir şeyle çakışıp çakışmadığı iki kez kontrol ediliyordu, karede 100000'den fazla. Tampon önbelleğinin zaten kontrol ettiği küçük okunan tamponlar artık bir kez kontrol ediliyor.
  - Bir çizimin descriptor'ları kayıt thread'i için paketlenirken her biri için memcpy çağrılmıyor, yerinde kopyalanıyor.
- RTX 5080 ve Ryzen 7 9800X3D'de, Seattle'daki parkta dururken, emülatör yüksek öncelikte, sırayla ölçüldü: 1.0.13'e göre üç turda ortalama 66.9 -> 70.1 FPS; işlemcinin %41-44'ünü başka programlar kullanıyordu. Orada artık GPU thread'i, ekran kartı ve oyunun ana thread'i sınırlarına yakın: GPU thread'i zamanının birkaç yüzdesinde oyunu bekliyor, ekran kartı her karenin 12-13 ms'sinde meşgul.
