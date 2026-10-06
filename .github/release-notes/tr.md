**Yenilikler**

- GPU'yu daha az bekleme. Çizim ve dispatch'lerin bir buffer'ın hangi kısmını kullandığı 256 bayta yuvarlanarak tutuluyordu; yan yana olanlar çakışıyor sanılıyor ve her birine, öncesindeki işi bekleyen bir barrier konuyordu. inFAMOUS Second Son her karede yan yana yazan yüzlerce minik dispatch çalıştırıyor ve hepsi bekliyordu. Erişimler artık bayt bayt tutuluyor.
- Log'daki GPU süre ölçümleri daha az command buffer'da yapılıyor; her biri için timestamp yazıp geri okumak hem GPU thread'inde hem GPU'da zaman alıyordu.
- RTX 5080 ve Ryzen 7 9800X3D'de, Seattle'daki parkta dururken ölçüldü: 65 -> 70 FPS, GPU'nun karede çalıştığı süre 14.0 -> 10.5 ms, karede barrier 1520 -> 1140, oyunun ana thread'inin GPU sonuçlarını bekleme payı %27 -> %15. Orada artık kare hızını GPU thread'i sınırlıyor; GPU'nun sınırladığı savaşlarda kazanç daha büyük olmalı. Lütfen bir savaştan log gönderin.
